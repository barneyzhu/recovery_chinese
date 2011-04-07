#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include <sys/vfs.h>

#include "extendedcommands.h"
#include "nandroid.h"

int print_and_error(const char* message) {
    ui_print("%s", message);
    return 1;
}

int yaffs_files_total = 0;
int yaffs_files_count = 0;
void yaffs_callback(char* filename)
{
    char* justfile = basename(filename);
    if (strlen(justfile) < 30)
        ui_print("%s", justfile);
    yaffs_files_count++;
    if (yaffs_files_total != 0)
        ui_set_progress((float)yaffs_files_count / (float)yaffs_files_total);
    ui_reset_text_col();
}

void compute_directory_stats(char* directory)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "find %s | wc -l > /tmp/dircount", directory);
    __system(tmp);
    char count_text[100];
    FILE* f = fopen("/tmp/dircount", "r");
    fread(count_text, 1, sizeof(count_text), f);
    fclose(f);
    yaffs_files_count = 0;
    yaffs_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    struct stat file_info;
    mkyaffs2image_callback callback = NULL;
    if (0 != stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info)) {
        callback = yaffs_callback;
    }
    
    ui_print("备份 %s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        ui_print("不能挂载 %s!\n", mount_point);
        return ret;
    }
    compute_directory_stats(mount_point);
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    ret = mkyaffs2image(mount_point, tmp, 0, callback);
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("制作此备份文件时候出错 %s!\n", mount_point);
        return ret;
    }
    return 0;
}

int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return NULL;

    // see if we need a raw backup (mtd)
    char tmp[PATH_MAX];
    int ret;
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        const char* name = basename(root);
        sprintf(tmp, "%s/%s.img", backup_path, name);
        ui_print("备份 %s 镜像...\n", name);
        if (0 != (ret = backup_raw_partition(vol->device, tmp))) {
            ui_print("制作此备份镜像文件时出错 %s !", name);
            return ret;
        }
        return 0;
    }

    return nandroid_backup_partition_extended(backup_path, root, 1);
}

int nandroid_backup(const char* backup_path)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    
    if (ensure_path_mounted("/sdcard") != 0)
        return print_and_error("Can't mount /sdcard\n");
    
    int ret;
    struct statfs s;
    if (0 != (ret = statfs("/sdcard", &s)))
        return print_and_error("Unable to stat /sdcard\n");
    uint64_t bavail = s.f_bavail;
    uint64_t bsize = s.f_bsize;
    uint64_t sdcard_free = bavail * bsize;
    uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
    ui_print("SD卡剩余空间: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < 150)
        ui_print("可能没有足够的空间完成备份... 继续...\n");
    
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);

    if (0 != (ret = nandroid_backup_partition(backup_path, "/boot")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/recovery")))
        return ret;

    Volume *vol = volume_for_path("/wimax");
    if (vol != NULL && 0 == stat(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        ui_print("Backing up WiMAX...\n");
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);
        ret = backup_raw_partition(vol->device, tmp);
        if (0 != ret)
            return print_and_error("Error while dumping WiMAX image!\n");
    }

    if (0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

    if (has_datadata()) {
        if (0 != (ret = nandroid_backup_partition(backup_path, "/datadata")))
            return ret;
    }

    if (0 != stat("/sdcard/.android_secure", &s))
    {
        ui_print("没有发现 /sdcard/.android_secure. 放弃备份SD卡上安装的程序.\n");
    }
    else
    {
        if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
            return ret;
    }

    if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    vol = volume_for_path("/sd-ext");
    if (vol == NULL || 0 != stat(vol->device, &s))
    {
        ui_print("没有发现sd-ext. 放弃备份sd-ext.\n");
    }
    else
    {
        if (0 != ensure_path_mounted("/sd-ext"))
            ui_print("不能挂载sd-ext. sd-ext备份可能不支持此设备. 放弃备份sd-ext.\n");
        else if (0 != (ret = nandroid_backup_partition(backup_path, "/sd-ext")))
            return ret;
    }

    ui_print("生成md5校验...\n");
    sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    if (0 != (ret = __system(tmp))) {
        ui_print("生成md5时出错!\n");
        return ret;
    }
    
    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\n备份完成!\n");
    return 0;
}

typedef int (*format_function)(char* root);

static void ensure_directory(const char* dir) {
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", dir);
    __system(tmp);
}

int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);
    
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (0 != (ret = statfs(tmp, &file_info))) {
        ui_print("%s.img没有发现. 放弃还原 %s.\n", name, mount_point);
        return 0;
    }

    ensure_directory(mount_point);

    unyaffs_callback callback = NULL;
    if (0 != stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info)) {
        callback = yaffs_callback;
    }

    ui_print("还原 %s...\n", name);
    /*
    if (0 != (ret = ensure_root_path_unmounted(root))) {
        ui_print("Can't unmount %s!\n", mount_point);
        return ret;
    }
    */
    if (0 != (ret = format_volume(mount_point))) {
        ui_print("格式化时出错 %s!\n", mount_point);
        return ret;
    }
    
    if (0 != (ret = ensure_path_mounted(mount_point))) {
        ui_print("不能挂载 %s!\n", mount_point);
        return ret;
    }
    
    if (0 != (ret = unyaffs(tmp, mount_point, callback))) {
        ui_print("还原时出错 %s!\n", mount_point);
        return ret;
    }

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    
    return 0;
}

int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw restore (mtd)
    char tmp[PATH_MAX];
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        int ret;
        const char* name = basename(root);
        ui_print("还原前擦除 %s ...\n", name);
        if (0 != (ret = format_volume(root))) {
            ui_print("擦除 %s 出错!", name);
            return ret;
        }
        sprintf(tmp, "%s%s.img", backup_path, root);
        ui_print("还原 %s 镜像...\n", name);
        if (0 != (ret = restore_raw_partition(vol->device, tmp))) {
            ui_print("写入 %s 出错!", name);
            return ret;
        }
        return 0;
    }
    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext, int restore_wimax)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    yaffs_files_total = 0;

    if (ensure_path_mounted("/sdcard") != 0)
        return print_and_error("Can't mount /sdcard\n");
    
    char tmp[PATH_MAX];

    ui_print("检查MD5校验...\n");
    sprintf(tmp, "cd %s && md5sum -c nandroid.md5", backup_path);
    if (0 != __system(tmp))
        return print_and_error("MD5不匹配!\n");
    
    int ret;

    if (restore_boot && NULL != volume_for_path("/boot") && 0 != (ret = nandroid_restore_partition(backup_path, "/boot")))
        return ret;
    
    struct stat s;
    Volume *vol = volume_for_path("/wimax");
    if (restore_wimax && vol != NULL && 0 == stat(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);

        struct stat st;
        if (0 != stat(tmp, &st))
        {
            ui_print("WARNING: WiMAX partition exists, but nandroid\n");
            ui_print("         backup does not contain WiMAX image.\n");
            ui_print("         You should create a new backup to\n");
            ui_print("         protect your WiMAX keys.\n");
        }
        else
        {
            ui_print("Erasing WiMAX before restore...\n");
            if (0 != (ret = format_volume("/wimax")))
                return print_and_error("Error while formatting wimax!\n");
            ui_print("Restoring WiMAX image...\n");
            if (0 != (ret = restore_raw_partition(vol->device, tmp)))
                return ret;
        }
    }

    if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        return ret;

    if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        return ret;
        
    if (has_datadata()) {
        if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
            return ret;
    }

    if (restore_data && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
        return ret;

    if (restore_cache && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (restore_sdext && 0 != (ret = nandroid_restore_partition(backup_path, "/sd-ext")))
        return ret;

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\n还原完成!\n");
    return 0;
}

void nandroid_generate_timestamp_path(char* backup_path)
{
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
    }
    else
    {
        strftime(backup_path, PATH_MAX, "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
    }
}

int nandroid_usage()
{
    printf("Usage: nandroid backup\n");
    printf("Usage: nandroid restore <directory>\n");
    return 1;
}

int nandroid_main(int argc, char** argv)
{
    if (argc > 3 || argc < 2)
        return nandroid_usage();
    
    if (strcmp("backup", argv[1]) == 0)
    {
        if (argc != 2)
            return nandroid_usage();
        
        char backup_path[PATH_MAX];
        nandroid_generate_timestamp_path(backup_path);
        return nandroid_backup(backup_path);
    }

    if (strcmp("restore", argv[1]) == 0)
    {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_restore(argv[2], 1, 1, 1, 1, 1, 0);
    }
    
    return nandroid_usage();
}
