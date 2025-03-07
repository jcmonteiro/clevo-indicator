/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libappindicator/app-indicator.h>

#define NAME "clevo-indicator"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define P775DM3 //THIS IS THE MODEL DEFINITION, TO FIND THE ADDRESSES IN THE EC

#define EC_REG_SIZE 0x100

#define EC_REG_CPU_FAN_DUTY 0xCE
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_CPU_FAN_RPMS_HI 0xD0
#define EC_REG_CPU_FAN_RPMS_LO 0xD1
#define EC_REG_GPU_FAN_RPMS_HI 0xD2
#define EC_REG_GPU_FAN_RPMS_LO 0xD3
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_GPU_FAN_DUTY 0xCF

#define MAX_FAN_RPM 4400.0

#define TEMP_FAIL_THRESHOLD 15

typedef enum {
    NA = 0, AUTO = 1, MANUAL = 2
} MenuItemType;

int use_hwmon_interface = 0;
int hwmon_interface_num = 0;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_cpu_fan(int duty_percentage);
static int main_test_gpu_fan(int duty_percentage);
static gboolean ui_update(gpointer user_data);
static void ui_command_set_fan(long fan_duty);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_cpu_fan_duty(void);
static int ec_query_cpu_fan_rpms(void);
static int ec_query_gpu_fan_duty(void);
static int ec_query_gpu_fan_rpms(void);
static int ec_write_cpu_fan_duty(int duty_percentage);
static int ec_write_gpu_fan_duty(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);

static AppIndicator* indicator = NULL;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;

}static menuitems[] = {
        { "Set FAN to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Set FAN to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, NULL },
        { "Set FAN to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, NULL },
        { "Set FAN to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, NULL },
        { "Set FAN to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, NULL },
        { "Set FAN to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Quit", G_CALLBACK(ui_command_quit), 0L, NA, NULL }
};

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int fan_duty;
    volatile int fan_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
}static *share_info = NULL;

static pid_t parent_pid = 0;

void autoset_cpu_gpu()
{
    struct sched_param param;
    param.sched_priority = 99;
    if (sched_setscheduler(0, SCHED_FIFO, & param) != 0)
    {
        printf("sched_setscheduler error\n");
        exit(EXIT_FAILURE);
    }

    int initial = 1;
    int current[2] = {0, 0};
    double lastCPU = 0., lastGPU = 0.;
    int repeatCheck[2] = {0, 0};
    int lastfail = 0;
    int missing = 0;

    fd_set readfds;
    FD_ZERO(&readfds);
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    FILE* ctrl_file = NULL;

    static int ctrl_setting_offset_cpu = 0;
    static int ctrl_setting_offset_gpu = 0;
    static int ctrl_setting_min_cpu = 0;
    static int ctrl_setting_min_gpu = 0;
    static int ctrl_setting_force_cpu = -1;
    static int ctrl_setting_force_gpu = -1;

    while (1)
    {
        //printf("Checking\n");
        if (missing > 5)
        {
            ec_write_gpu_fan_duty(70);
            ec_write_cpu_fan_duty(70);
            exit(1);
        }

        char buffer[10];
        double gputemp;
        int found = 0;
        while (!feof(stdin))
        {
            FD_SET(STDIN_FILENO, &readfds);
            if (!select(1, &readfds, NULL, NULL, &timeout)) break;
            int nRead = read(STDIN_FILENO, buffer, 3);
            //printf("Read %d\n", nRead);
            if (nRead == 0) break;
            found = 1;
            gputemp = atoi(buffer);
        };

        if (found)
        {
            static int ctrl_check = 0;
            if (ctrl_check++ >= 3)
            {
                ctrl_check = 0;
                ctrl_file = fopen("/tmp/clevo_fan_ctrl", "r");
                if (ctrl_file != NULL)
                {
                    while (!feof(ctrl_file))
                    {
                        char buffer[1024];
                        fgets(buffer, 1023, ctrl_file);
                        if (strncmp(buffer, "offset_cpu", 10) == 0) sscanf(buffer, "offset_cpu %d", &ctrl_setting_offset_cpu);
                        if (strncmp(buffer, "offset_gpu", 10) == 0) sscanf(buffer, "offset_gpu %d", &ctrl_setting_offset_gpu);
                        if (strncmp(buffer, "min_cpu", 7) == 0) sscanf(buffer, "min_cpu %d", &ctrl_setting_min_cpu);
                        if (strncmp(buffer, "min_gpu", 7) == 0) sscanf(buffer, "min_gpu %d", &ctrl_setting_min_gpu);
                        if (strncmp(buffer, "force_cpu", 7) == 0) sscanf(buffer, "force_cpu %d", &ctrl_setting_force_cpu);
                        if (strncmp(buffer, "force_gpu", 7) == 0) sscanf(buffer, "force_gpu %d", &ctrl_setting_force_gpu);
                    }
                    printf("Control settings: Offset CPU %d, Offset GPU %d, Min CPU %d, Min GPU %d, Force CPU %d, Force GPU %d (hwmon %d)\n", ctrl_setting_offset_cpu, ctrl_setting_offset_gpu, ctrl_setting_min_cpu, ctrl_setting_min_gpu, ctrl_setting_force_cpu, ctrl_setting_force_gpu, use_hwmon_interface);
                    fclose(ctrl_file);
                }
            }
            double cputemp;
            for (int tries = 0;tries < 3;tries++)
            {
                cputemp = ec_query_cpu_temp();
                if (cputemp < 100 || cputemp < lastCPU + 20) break;
            }
            if (cputemp < lastCPU - 10) cputemp = lastCPU - 10;

            int cur_cpu_setting = ec_query_cpu_fan_duty();
            int cur_gpu_setting = ec_query_gpu_fan_duty();

            double gputemptmp;
            if (gputemp <= 65) gputemptmp = gputemp - 10;
            else if (gputemp < 75) gputemptmp = gputemp - (75 - gputemp);
            else gputemptmp = gputemp;

            double avg[2];
            if (cputemp > gputemptmp)
            {
                avg[0] = cputemp;
                avg[1] = (2 * gputemptmp + cputemp) / 3;
            }
            else
            {
                avg[1] = gputemptmp;
                avg[0] = (2 * cputemp + gputemptmp) / 3;
            }
            if (lastCPU > 30) avg[0] = (2 * avg[0] + lastCPU) / 3;
            if (lastGPU > 30) avg[1] = (2 * avg[1] + lastGPU) / 3;
            if (cputemp >= TEMP_FAIL_THRESHOLD) lastCPU = avg[0];
            if (gputemp >= TEMP_FAIL_THRESHOLD) lastGPU = avg[1];

            int setDuty[2];
            for (int i = 0;i < 2;i++)
            {
                if (avg[i] <= 40) setDuty[i] = 0;
                else if (avg[i] <= 45) setDuty[i] = 15;
                else if (avg[i] <= 75) setDuty[i] = avg[i] - 30;
                else if (avg[i] <= 90) setDuty[i] = (avg[i] - 75) * 3 + 45;
                else setDuty[i] = 100;
            }

            if (ctrl_setting_offset_cpu) setDuty[0] += ctrl_setting_offset_cpu;
            if (ctrl_setting_offset_gpu) setDuty[1] += ctrl_setting_offset_gpu;
            if (ctrl_setting_min_cpu > setDuty[0]) setDuty[0] = ctrl_setting_min_cpu;
            if (ctrl_setting_min_gpu > setDuty[1]) setDuty[1] = ctrl_setting_min_gpu;
            if (ctrl_setting_force_cpu != -1) setDuty[0] = ctrl_setting_force_cpu;
            if (ctrl_setting_force_gpu != -1) setDuty[1] = ctrl_setting_force_gpu;
            for (int i = 0;i < 2;i++) if (setDuty[i] > 100) setDuty[i] = 100;

            int doSet[2] = {0, 0};
            for (int i = 0;i < 2;i++)
            {
                if (current[i] == 0 && setDuty[i] != 0) doSet[i] = 1;
                else if (setDuty[i] > current[i] && setDuty[i] > 50) doSet[i] = 1;
                else if (setDuty[i] > current[i] + 1) doSet[i] = 1;
                else if (setDuty[i] < current[i] - 5) doSet[i] = 1;
                else if (setDuty[i] < current[i])
                {
                    if (repeatCheck[i] >= 4)
                        doSet[i] = 1;
                    else
                        repeatCheck[i]++;
                }

                if (doSet[i]) repeatCheck[i] = 0;
            }

            if (initial)
            {
                doSet[0] = doSet[1] = 1;
                initial = 0;
            }
            else if (cur_cpu_setting != current[0] || cur_gpu_setting != current[1])
            {
                doSet[0] = doSet[1] = 1;
                for (int i = 0;i < 2;i++) if (setDuty[i] < current[i]) setDuty[i] = current[i];
            }

            if (cputemp < TEMP_FAIL_THRESHOLD || gputemp < TEMP_FAIL_THRESHOLD)
            {
                if (lastfail >= 1)
                {
                    doSet[0] = doSet[1] = 1;
                    if (setDuty[0] < 50) setDuty[0] = 50;
                    if (setDuty[1] < 50) setDuty[1] = 50;
                }
                else
                {
                    lastfail++;
                    doSet[0] = doSet[1] = 0;
                }
            }
            else
            {
                lastfail = 0;
            }

            printf("Temperatures C: %f G: %f --> %f %f --> New Duty: %d (%d) %d (%d) - Activate %d %d\n", cputemp, gputemp, avg[0], avg[1], setDuty[0], cur_cpu_setting, setDuty[1], cur_gpu_setting, doSet[0], doSet[1]);

            for (int i = 0;i < 2;i++)
            {
                if (doSet[i])
                {
                    current[i] = setDuty[i];
                    int retVal;
                    for (int j = 0;j < 3;j++)
                    {
                        if (i) retVal = ec_write_gpu_fan_duty(setDuty[1]);
                        else retVal = ec_write_cpu_fan_duty(setDuty[0]);
                        if (retVal == EXIT_SUCCESS)
                        {
                            usleep(1100000);
                            int new_setting = i ? ec_query_gpu_fan_duty() : ec_query_cpu_fan_duty();
                            if (new_setting == setDuty[i]) break;
                            printf("Mismatch %d : %d v.s. %d\n", i, new_setting, setDuty[i]);
                        }
                        printf("Error setting speed, retrying...\n");
                        usleep(50000);
                    }
                }
            }
            missing = 0;
        }
        else
        {
            missing++;
        }
        usleep(1000000);
    };
}

int main(int argc, char* argv[]) {
    printf("Simple fan control utility for Clevo laptops\n");
    if (check_proc_instances(NAME) > 1) {
        printf("Multiple running instances!\n");
        char* display = getenv("DISPLAY");
        if (display != NULL && strlen(display) > 0) {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            //
            gtk_init(&argc, &argv);
            GtkWidget* dialog = gtk_message_dialog_new(NULL, 0,
                    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Multiple running instances of %s!", NAME);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return EXIT_FAILURE;
    }
    if (ec_init() != EXIT_SUCCESS) {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (argc <= 1 || strcmp(argv[1], "help") == 0) {
        printf(
                    "\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fan duty on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tTarget fan duty in percentage, from 60 to 100\n\
  -?\t\t\t\tDisplay this help and exit\n\
\n\
Without arguments this program should attempt to display an indicator in\n\
the Ubuntu tray area for fan information display and control. The indicator\n\
requires this program to have setuid=root flag but run from the desktop user\n\
, because a root user is not allowed to display a desktop indicator while a\n\
non-root user is not allowed to control Clevo EC (Embedded Controller that's\n\
responsible of the fan). Fix permissions of this executable if it fails to\n\
run:\n\
    sudo chown root clevo-indicator\n\
    sudo chmod u+s  clevo-indicator\n\
\n\
Note any fan duty change should take 1-2 seconds to come into effect - you\n\
can verify by the fan speed displayed on indicator icon and also louder fan\n\
noise.\n\
\n\
In the indicator mode, this program would always attempt to load kernel\n\
module 'ec_sys', in order to query EC information from\n\
'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n\
which may be more risky if interrupted or concurrently operated during the\n\
process.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n");
        return main_dump_fan();
    }
    else if (strcmp(argv[1], "indicator") == 0) {
        char* display = getenv("DISPLAY");
        if (display == NULL || strlen(display) == 0) {
            return main_dump_fan();
        } else {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            pid_t worker_pid = fork();
            if (worker_pid == 0) {
                signal(SIGCHLD, SIG_DFL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            } else if (worker_pid > 0) {
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, NULL, 0);
            } else {
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    } else if (strcmp(argv[1], "set") == 0) {
        if (argc < 2) {
            printf("Missing argument\n");
            return EXIT_FAILURE;
        } else {
            int val = atoi(argv[2]);
            if (val < 0 || val > 100)
                    {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_cpu_fan(val);
        }
    } else if (strcmp(argv[1], "setg") == 0) {
        if (argc < 2) {
            printf("Missing argument\n");
            return EXIT_FAILURE;
        } else {
            int val = atoi(argv[2]);
            if (val < 0 || val > 100)
                    {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_gpu_fan(val);
        }
    } else if (strcmp(argv[1], "dump") == 0) {
        return main_dump_fan();
    } else if (strcmp(argv[1], "dumpall") == 0) {
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            return EXIT_FAILURE;
        case 0x100:
            break;
        default:
            printf("Error reading from EC\n");
            return EXIT_FAILURE;
        }

        for (int i = 0;i < EC_REG_SIZE;i++) {
            printf("0x%02x: 0x%02x (%3d) ", i, buf[i], buf[i]);
            if (i == EC_REG_CPU_TEMP) printf("C");
            else if (i ==  EC_REG_GPU_TEMP) printf("G");
            else if (i == EC_REG_CPU_FAN_DUTY) printf("F");
            else if (i == EC_REG_CPU_FAN_RPMS_HI) printf("H");
            else if (i == EC_REG_CPU_FAN_RPMS_LO) printf("L");
            else if (buf[i] >= 51 && buf[i] <= 51) printf("X");
            else printf(" ");
            printf(" ");
            if ((i + 1) % 16 == 0) printf("\n");
        }
        close(io_fd);
    }
    else if (strcmp(argv[1], "auto") == 0) {
        if (getenv("USE_HWMON") && strcmp(getenv("USE_HWMON"), "1") == 0)
        {
            use_hwmon_interface = 1;
            int i = 0;
            int foundif = 0;
            while (1)
            {
                FILE* fp;
                char name[1024];
                sprintf(name, "/sys/class/hwmon/hwmon%d/name", i);
                fp = fopen(name, "rb");
                if (fp == 0) break;
                fgets(name, 1023, fp);
                fclose(fp);
                if (strcmp(name, "clevo_xsm_wmi\n") == 0)
                {
                    printf("Found clevo interface %d %s", i, name);
                    hwmon_interface_num = i;
                    foundif = 1;
                    break;
                }
                i++;
            };
            if (!foundif) return EXIT_FAILURE;
        }
        autoset_cpu_gpu();
    }

    return EXIT_SUCCESS;
}

static void main_init_share(void) {
    void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
            -1, 0);
    share_info = shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->fan_duty = 0;
    share_info->fan_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_duty_val = 0;
    share_info->manual_next_fan_duty = 0;
    share_info->manual_prev_fan_duty = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    system("modprobe ec_sys");
    FILE* io_fd = fopen("/sys/kernel/debug/ec/ec0/io", "r");
    if (io_fd <= 0)
    {
        printf("unable to read EC from sysfs: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    while (share_info->exit == 0 && io_fd > 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC
        int new_fan_duty = share_info->manual_next_fan_duty;
        if (new_fan_duty != 0
                && new_fan_duty != share_info->manual_prev_fan_duty) {
            ec_write_cpu_fan_duty(new_fan_duty);
            share_info->manual_prev_fan_duty = new_fan_duty;
        }
        // read EC
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = fread(buf, 1, EC_REG_SIZE, io_fd);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = buf[EC_REG_GPU_TEMP];
            share_info->fan_duty = calculate_fan_duty(buf[EC_REG_CPU_FAN_DUTY]);
            share_info->fan_rpms = calculate_fan_rpms(buf[EC_REG_CPU_FAN_RPMS_HI],
                    buf[EC_REG_CPU_FAN_RPMS_LO]);
            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        // auto EC
        if (share_info->auto_duty == 1) {
            int next_duty = ec_auto_duty_adjust();
            if (next_duty != 0 && next_duty != share_info->auto_duty_val) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf("%s CPU=%d°C, GPU=%d°C, auto fan duty to %d%%\n", s_time,
                        share_info->cpu_temp, share_info->gpu_temp, next_duty);
                ec_write_cpu_fan_duty(next_duty);
                share_info->auto_duty_val = next_duty;
            }
        }
        //
        fclose(io_fd);
        usleep(200 * 1000);
        io_fd = fopen("/sys/kernel/debug/ec/ec0/io", "r");
    }
    if (io_fd > 0)
        fclose(io_fd);
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv) {
    printf("Indicator...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);
    //
    gtk_init(&argc, &argv);
    //
    GtkWidget* indicator_menu = gtk_menu_new();
    for (int i = 0; i < menuitem_count; i++) {
        GtkWidget* item;
        if (strlen(menuitems[i].label) == 0) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(menuitems[i].label);
            g_signal_connect_swapped(item, "activate",
                    G_CALLBACK(menuitems[i].callback),
                    (void* ) menuitems[i].option);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);
    //
    indicator = app_indicator_new(NAME, "fan",
            APP_INDICATOR_CATEGORY_HARDWARE);
    g_assert(IS_APP_INDICATOR(indicator));
    app_indicator_set_label(indicator, "Init..", "XX");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
    char icon_name[] = "indicator_fan.svg";
    app_indicator_set_icon(indicator, icon_name);
    g_timeout_add(500, &ui_update, NULL);
    ui_toggle_menuitems(share_info->fan_duty);
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum) {
    printf("main on worker quit signal\n");
    exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum) {
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void) {
    printf("Dump fan information\n");
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  CPUFAN Duty: %d%%\n", ec_query_cpu_fan_duty());
    printf("  CPUFAN RPMs: %d RPM\n", ec_query_cpu_fan_rpms());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    printf("  GPU FAN Duty: %d%%\n", ec_query_gpu_fan_duty());
    printf("  GPU RPMs: %d RPM\n", ec_query_gpu_fan_rpms());
    return EXIT_SUCCESS;
}

static int main_test_cpu_fan(int duty_percentage) {
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_cpu_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static int main_test_gpu_fan(int duty_percentage) {
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_gpu_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data) {
    char label[256];
    sprintf(label, "%d℃ %d℃", share_info->cpu_temp, share_info->gpu_temp);
    app_indicator_set_title(indicator, label);
    return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    if (fan_duty_val == 0) {
        printf("clicked on fan duty auto\n");
        share_info->auto_duty = 1;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = 0;
    } else {
        printf("clicked on fan duty: %d\n", fan_duty_val);
        share_info->auto_duty = 0;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = fan_duty_val;
    }
    ui_toggle_menuitems(fan_duty_val);
}

static void ui_command_quit(gchar* command) {
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty) {
    for (int i = 0; i < menuitem_count; i++) {
        if (menuitems[i].widget == NULL)
            continue;
        if (fan_duty == 0)
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != MANUAL
                            || (int) menuitems[i].option != fan_duty);
    }
}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
}

static int ec_auto_duty_adjust(void) {
    int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
    int duty = share_info->fan_duty;
    //
    if (temp >= 80 && duty < 100)
        return 100;
    if (temp >= 70 && duty < 90)
        return 90;
    if (temp >= 60 && duty < 80)
        return 80;
    if (temp >= 55 && duty < 70)
        return 70;
    if (temp >= 40 && duty < 60)
        return 60;
    if (temp >= 30 && duty < 50)
        return 50;
    if (temp >= 20 && duty < 40)
        return 40;
    if (temp >= 10 && duty < 30)
        return 30;
    //
    if (temp <= 15 && duty > 30)
        return 30;
    if (temp <= 25 && duty > 40)
        return 40;
    if (temp <= 35 && duty > 50)
        return 50;
    if (temp <= 45 && duty > 60)
        return 60;
    if (temp <= 55 && duty > 70)
        return 70;
    if (temp <= 65 && duty > 80)
        return 80;
    if (temp <= 75 && duty > 90)
        return 90;
    //
    return 100;
}

static int ec_query_cpu_temp(void) {
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/temp1_input", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "rb");
        if (fp == 0) return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return(val / 1000);
    }
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_cpu_fan_duty(void) {
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm1", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "rb");
        if (fp == 0) return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return calculate_fan_duty(val);
    }
    int raw_duty = ec_io_read(EC_REG_CPU_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_cpu_fan_rpms(void) {
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/fan1_input", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "rb");
        if (fp == 0) return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return val;
    }
    int raw_rpm_hi = ec_io_read(EC_REG_CPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_CPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_gpu_fan_duty(void) {
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm2", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "rb");
        if (fp == 0) return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return calculate_fan_duty(val);
    }
    int raw_duty = ec_io_read(EC_REG_GPU_FAN_DUTY);
     return calculate_fan_duty(raw_duty);
}

static int ec_query_gpu_fan_rpms(void) {
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/fan2_input", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "rb");
        if (fp == 0) return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return val;
    }
    int raw_rpm_hi = ec_io_read(EC_REG_GPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_GPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_cpu_fan_duty(int duty_percentage) {
    if (duty_percentage < 0 || duty_percentage > 100) {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0 + 0.5;
    int v_i = (int) v_d;
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm1", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "wb");
        if (fp == 0) return 99;
        fprintf(fp, "%d\n", v_i);
        fclose(fp);
        return 0;
    }
    return ec_io_do(0x99, 0x01, v_i);
}

static int ec_write_gpu_fan_duty(int duty_percentage) {
    if (duty_percentage < 0 || duty_percentage > 100) {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0 + 0.5;
    int v_i = (int) v_d;
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm2", hwmon_interface_num);
        FILE* fp;
        fp = fopen(name, "wb");
        if (fp == 0) return 99;
        fprintf(fp, "%d\n", v_i);
        fclose(fp);
        return 0;
    }
    return ec_io_do(0x99, 0x02, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    if (i >= 1000) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0 + 0.5);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char* proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR* dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        char* endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                        && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char* buffer, size_t max, const char* format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler) {
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}
