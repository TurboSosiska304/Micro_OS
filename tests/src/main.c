/*
 * Micro OS — unit test runner.
 *
 * The tests run inside a kernel task: this way they exercise the very same
 * task API that the applications on the device will use.
 */
#include <stdatomic.h>

#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <unistd.h>

#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

static atomic_bool g_done;
static int g_result;

static void run_tests(void *arg)
{
    K_UNUSED(arg);

    test_version();
    test_board();
    test_log();
    test_ansi();
    test_port();
    test_blockdev();
    test_blockdev_cache();
    test_partition();
    test_root();
    test_render();
    test_task();
    test_sched();
    test_sched_queue();
    test_smp();
    test_kmod();
    test_kmm();
    test_exec();
    test_fuzz();
    test_mfs();
    test_mfs_persistence();
    test_mfs_repair();
    test_mfs_many();
    test_mfs_chain();
    test_mfs_big();
    test_mkfs_image();
    test_vfs();
    test_fs_fat();
    test_fs_ext2();
    test_sysfs();
    test_ui();
    test_input_debounce();
    test_battery_soc();
    test_fb_draw();
    test_rtc();
    test_ir_nec();
    test_sound_detect();
    test_console_sinks();
    test_telnet();
    test_text_wrap();
    test_ksys();
    test_ksys_process();
    test_users();
    test_libc();
    test_kshell();
    test_kshell_pipe();
    test_initd();
    test_config();
    test_settings();
#if MICRO_OS_NET && MICRO_OS_NET_HTTP && MICRO_OS_NET_HTTP_SERVER && MICRO_OS_NET_NTP &&           \
    MICRO_OS_NET_MQTT
    test_net();
    test_pkg();
#if MICRO_OS_TLS && MICRO_OS_NET
    test_tls();
#endif
#endif
    test_boot();
#if MICRO_OS_WIFI
    test_wifi();
#endif
    test_sign();
    test_progs();
    test_zlib();
    test_pcre2();
    test_lua();
    test_zstd();
    test_awk();
    test_userland_float();
    test_bc();
    test_file();
    test_sqlite();

    g_result = ktest_summary();
    atomic_store_explicit(&g_done, true, memory_order_release);
}

int main(void)
{
    struct task *runner;

    printf("Micro OS unit tests\n\n");

    k_sched_init();
    port_host_start_tick(1);

    runner = task_create("test-runner", run_tests, NULL, TASK_PRIO_DEFAULT);
    if (runner == NULL) {
        printf("cannot create the test runner task\n");
        return 1;
    }

    /*
     * A run that does not end is a failed run: the runner stops itself
     * instead of leaving the CI to wait forever.
     */
    {
        const useconds_t limit = 120u * 1000u * 1000u;
        useconds_t waited = 0;

        while (!atomic_load_explicit(&g_done, memory_order_acquire) && waited < limit) {
            usleep(1000);
            waited += 1000;
        }

        if (!g_done) {
            printf("\nthe tests did not end in %u second(s), stopping\n", 120u);
            g_result = 2;
        }
    }

    port_host_stop_tick();

    return g_result;
}
