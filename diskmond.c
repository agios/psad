/*
*****************************************************************************
*
*  File: diskmond.c
*
*  Purpose: diskmond checks the disk partition on which psad scan data
*           lives and will remove the data if the disk utilization goes
*           above a (configurable) threshold (95% by default).
*
*  Author: Michael Rash (mbr@cipherdyne.com)
*
*  Credits:  (see the CREDITS file)
*
*  Version: 1.1.1
*
*  Copyright (C) 1999-2001 Michael Rash (mbr@cipherdyne.com)
*
*  License (GNU Public License):
*
*     This program is distributed in the hope that it will be useful,
*     but WITHOUT ANY WARRANTY; without even the implied warranty of
*     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*     GNU General Public License for more details.
*
*     You should have received a copy of the GNU General Public License
*     along with this program; if not, write to the Free Software
*     Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
*     USA
*
*****************************************************************************
*
*  $Id$
*/

/* includes */
#include "psad.h"
#include <sys/vfs.h> /* statfs() */
#include <dirent.h>
#include <ctype.h>   /* isdigit() */

/* defines */
#define DISKMOND_CONF "/etc/psad/diskmond.conf"

/* globals */
short int email_ctr = 0;
const char mail_redr[] = " < /dev/null > /dev/null 2>&1";
const char hostname[] = HOSTNAME;
char mail_addrs[MAX_GEN_LEN];
char shCmd[MAX_GEN_LEN];
char mailCmd[MAX_GEN_LEN];
static volatile sig_atomic_t received_sighup = 0;

/* prototypes */
static void parse_config(
    char *config_file,
    char *psad_dir,
    char *fwdata_file,
    char *archive_dir,
    char *shCmd,
    char *mailCmd,
    char *mail_addrs,
    char *diskmond_pid_file,
    unsigned short int *max_disk_percentage,
    unsigned int *diskmond_check_interval,
    unsigned int *diskmond_max_retries
);

void rm_data(char *fwdata_file, char *psad_dir, char *archive_dir);
void rm_scanlog(char *dir);
int check_ip_dir(char *file);
static void sighup_handler(int sig);

/* main */
int main(int argc, char *argv[]) {
    char config_file[MAX_PATH_LEN];
    char psad_dir[MAX_PATH_LEN];
    char fwdata_file[MAX_PATH_LEN];
    char archive_dir[MAX_PATH_LEN];
    char diskmond_pid_file[MAX_PATH_LEN];
    float current_prct = 0;
    unsigned short int max_disk_percentage = 95; /* default to 95% utilization */
    unsigned int diskmond_check_interval   = 5;  /* default to 5 seconds */
    unsigned int diskmond_max_retries      = 10; /* default to 10 tries */
    struct statfs statfsbuf;

#ifdef DEBUG
    printf(" .. Entering DEBUG mode ..\n");
    sleep(1);
#endif

    /* handle command line arguments */
    if (argc == 1) {  /* nothing but the program name was
                         specified on the command line */
        strlcpy(config_file, DISKMOND_CONF, MAX_PATH_LEN);
    } else if (argc == 2) {  /* the path to the config file was
                                supplied on the command line */
        strlcpy(config_file, argv[1], MAX_PATH_LEN);
    } else {
        printf(" .. You may only specify the path to a single config file:  ");
        printf("Usage:  diskmond <configfile>\n");
        exit(EXIT_FAILURE);
    }

#ifdef DEBUG
    printf(" .. parsing config_file: %s\n", config_file);
#endif

    /* parse the config file */
    parse_config(
        config_file,
        psad_dir,
        fwdata_file,
        archive_dir,
        shCmd,
        mailCmd,
        mail_addrs,
        diskmond_pid_file,
        &max_disk_percentage,
        &diskmond_check_interval,
        &diskmond_max_retries
    );

    /* first make sure there isn't another diskmond already running */
    check_unique_pid(diskmond_pid_file, "diskmond");

#ifndef DEBUG
    /* become a daemon */
    daemonize_process(diskmond_pid_file);
#endif

    /* install signal handler for HUP signals */
    signal(SIGHUP, sighup_handler);

    /* start doing the real work now that the daemon is running and
     * the config file has been parsed */

    if (chdir(psad_dir) < 0) {
#ifdef DEBUG
    printf(" ** Could not chdir() into: %s\n", psad_dir);
#endif
        exit(EXIT_FAILURE);
    }

    /* MAIN LOOP */
    for (;;) {
        if (!statfs(psad_dir, &statfsbuf)) {
            current_prct =
                (float) statfsbuf.f_bfree / statfsbuf.f_blocks * 100;
            if (current_prct > max_disk_percentage) {
                rm_data(fwdata_file, psad_dir, archive_dir);
            }
        }

        /* sleep and then see if we received any signals */
        sleep(diskmond_check_interval);

        if (received_sighup) {

            /* clear the HUP flag */
            received_sighup = 0;
#ifdef DEBUG
    printf(" .. re-parsing config file: %s\n", config_file);
#endif
            /* reparse the config file since we received a
             * HUP signal */
            parse_config(
                config_file,
                psad_dir,
                fwdata_file,
                archive_dir,
                shCmd,
                mailCmd,
                mail_addrs,
                diskmond_pid_file,
                &max_disk_percentage,
                &diskmond_check_interval,
                &diskmond_max_retries
            );
            slogr("psad(diskmond)",
                    "Received HUP signal, re-imported diskmond.conf");
        }
    }

    /* this statement doesn't get executed, but for completeness... */
    exit(EXIT_SUCCESS);
}
/******************** end main ********************/

static void parse_config(
    char *config_file,
    char *psad_dir,
    char *fwdata_file,
    char *archive_dir,
    char *shCmd,
    char *mailCmd,
    char *mail_addrs,
    char *diskmond_pid_file,
    unsigned short int *max_disk_percentage,
    unsigned int *diskmond_check_interval,
    unsigned int *diskmond_max_retries)
{
    FILE *config_ptr;         /* FILE pointer to the config file */
    int linectr = 0;
    char config_buf[MAX_LINE_BUF];
    char char_max_disk_percentage[MAX_NUM_LEN];
    char char_diskmond_check_interval[MAX_NUM_LEN];
    char char_diskmond_max_retries[MAX_NUM_LEN];
    char *index;

    if ((config_ptr = fopen(config_file, "r")) == NULL) {
        perror(" ** Could not open config file");
        exit(EXIT_FAILURE);
    }

    /* increment through each line of the config file */
    while ((fgets(config_buf, MAX_LINE_BUF, config_ptr)) != NULL) {
        linectr++;
        index = config_buf;  /* set the index pointer to the
                                beginning of the line */

        /* advance the index pointer through any
         * whitespace at the beginning of the line */
        while (*index == ' ' || *index == '\t') index++;

        /* skip comments and blank lines, etc. */
        if ((*index != '#') && (*index != '\n')
                && (*index != ';') && (index != NULL)) {

            find_char_var("PSAD_DIR", psad_dir, index);
            find_char_var("FW_DATA_FILE", fwdata_file, index);
            find_char_var("SCAN_DATA_ARCHIVE_DIR", archive_dir, index);
            find_char_var("shCmd ", shCmd, index);
            find_char_var("mailCmd ", mailCmd, index);
            find_char_var("EMAIL_ADDRESSES ", mail_addrs, index);
            find_char_var("DISKMOND_PID_FILE ", diskmond_pid_file, index);
            find_char_var("MAX_DISK_PERCENTAGE ",
                char_max_disk_percentage, index);
            find_char_var("DISKMOND_CHECK_INTERVAL ",
                char_diskmond_check_interval, index);
            find_char_var("DISKMOND_MAX_RETRIES ",
                char_diskmond_max_retries, index);
            find_char_var("DISKMOND_PID_FILE ", diskmond_pid_file, index);
        }
    }
    *max_disk_percentage     = atoi(char_max_disk_percentage);
    *diskmond_check_interval = atoi(char_diskmond_check_interval);
    *diskmond_max_retries    = atoi(char_diskmond_max_retries);
    fclose(config_ptr);
    return;
}

void rm_data(char *fwdata_file, char *psad_dir, char *archive_dir) {
    char mail_str[MAX_MSG_LEN] = "";
    char path_tmp[MAX_PATH_LEN];
    int fd;

    /* remove the ip/scanlog files in psad_dir and archive_dir
     * since they take up the most space */
    rm_scanlog(psad_dir);
    rm_scanlog(archive_dir);

    if (chdir(psad_dir) < 0) {
#ifdef DEBUG
    printf(" ** Could not chdir() into: %s\n", psad_dir);
#endif
        exit(EXIT_FAILURE);
    }

    /* truncate the fwdata_file after truncating the scanlog
     * files since changing the number of lines in fwdata will
     * cause psad to write to files in the ip directories */
    if ((fd = open(fwdata_file, O_TRUNC)) >= 0)
        close(fd);

    strlcpy(path_tmp, archive_dir, MAX_PATH_LEN);
    strlcat(path_tmp, "/fwdata_archive", MAX_PATH_LEN);

    if ((fd = open(path_tmp, O_TRUNC)) >= 0)
        close(fd);

#ifdef DEBUG
    printf(" .. sending mail:  %s\n", mail_str);
#endif

    strlcat(mail_str,
            " -s \" ** psad diskmond: Removing data in ", MAX_MSG_LEN);
    strlcat(mail_str, psad_dir, MAX_MSG_LEN);
    strlcat(mail_str, " on ", MAX_MSG_LEN);
    strlcat(mail_str, hostname, MAX_MSG_LEN);
    strlcat(mail_str, "\" ", MAX_MSG_LEN);
    strlcat(mail_str, mail_addrs, MAX_MSG_LEN);
    strlcat(mail_str, mail_redr, MAX_MSG_LEN);

    /* send alert email */
    send_alert_email(shCmd, mailCmd, mail_str);
    return;
}

void rm_scanlog(char *dir) {
    DIR *dir_ptr;
    struct dirent *direntp;
    char path_tmp[MAX_PATH_LEN];
    int fd;

    if ((dir_ptr = opendir(dir)) == NULL) {
        /* could not open dir */
        return;
    } else {
        if (chdir(dir) < 0) {
#ifdef DEBUG
    printf(" ** Could not chdir(%s)\n", dir);
#endif
            closedir(dir_ptr);
            return;
        }
        while ((direntp = readdir(dir_ptr)) != NULL) {
            if (check_ip_dir(direntp->d_name)) {
                strlcpy(path_tmp, direntp->d_name, MAX_PATH_LEN);
                strlcat(path_tmp, "/scanlog", MAX_PATH_LEN);
                /* zero out the file */
                if ((fd = open(path_tmp, O_TRUNC)) >= 0)
                    close(fd);
            }
        }
        closedir(dir_ptr);
    }
    return;
}

int check_ip_dir(char *file) {
    int pctr = 0;
    char *index;
    if (isdigit(file[0])) {
        index = file;
        while (*index != '\0') {
            if (*index == '.') {
                pctr++;
            }
            index++;
        }
        /* ex: 10.10.10.10 */
        if (pctr == 3) {
            return 1;
        }
    }
    return 0;
}

static void sighup_handler(int sig)
{
    received_sighup = 1;
}
