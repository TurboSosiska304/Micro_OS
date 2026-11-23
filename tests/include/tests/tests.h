/*
 * Micro OS — unit test cases.
 */
#ifndef MICRO_OS_TESTS_H
#define MICRO_OS_TESTS_H

void test_log(void);
void test_port(void);
void test_blockdev(void);
void test_partition(void);
void test_render(void);
void test_boot(void);
void test_version(void);
void test_board(void);
void test_task(void);
void test_sched(void);
void test_sched_queue(void);
void test_smp(void);
void test_kmod(void);
void test_kmm(void);
void test_exec(void);
void test_fuzz(void);
void test_mfs(void);
void test_mfs_persistence(void);
void test_mfs_repair(void);
void test_mfs_many(void);
void test_mkfs_image(void);
void test_vfs(void);
void test_fs_fat(void);
void test_fs_ext2(void);
void test_sysfs(void);
void test_input_debounce(void);
void test_battery_soc(void);
void test_fb_draw(void);
void test_rtc(void);
void test_ir_nec(void);
void test_sound_detect(void);
void test_console_sinks(void);
void test_telnet(void);
void test_text_wrap(void);
void test_ksys(void);
void test_ksys_process(void);
void test_libc(void);
void test_kshell(void);
void test_kshell_pipe(void);
void test_initd(void);
void test_config(void);
void test_settings(void);
void test_net(void);
void test_pkg(void);
void test_wifi(void);
void test_sign(void);
void test_progs(void);
void test_zlib(void);
void test_lua(void);
void test_zstd(void);
void test_userland_float(void);
void test_file(void);

#endif /* MICRO_OS_TESTS_H */
