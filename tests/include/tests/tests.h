/*
 * Micro OS — unit test cases.
 */
#ifndef MICRO_OS_TESTS_H
#define MICRO_OS_TESTS_H

void test_log(void);
void test_boot(void);
void test_version(void);
void test_board(void);
void test_task(void);
void test_sched(void);
void test_sched_queue(void);
void test_kmod(void);
void test_kmm(void);
void test_mfs(void);
void test_mfs_persistence(void);
void test_vfs(void);
void test_sysfs(void);
void test_input_debounce(void);
void test_battery_soc(void);
void test_fb_draw(void);
void test_ksys(void);
void test_ksys_process(void);
void test_libc(void);
void test_kshell(void);
void test_wifi(void);

#endif /* MICRO_OS_TESTS_H */
