/*
   Copyright (C) 2014-2015, Rudolf Sikorski <rudolf.sikorski@freenet.de>

   This file is part of the `drwebmirror' program.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "drwebmirror.h"
#include <sys/stat.h>
#include <utime.h>
#include <dirent.h>

/* Set modification time <mtime> to file <filename> */
int set_mtime(const char * filename, const time_t mtime)
{
    struct stat f_stat;
    struct utimbuf new_times;

    if(stat(filename, & f_stat) < 0)
    {
        fprintf(ERRFP, "Error %d with stat() on %s: %s\n", errno, filename, strerror(errno));
        return EXIT_FAILURE;
    }

    new_times.actime = f_stat.st_atime;
    new_times.modtime = mtime;

    if(utime(filename, & new_times) < 0)
    {
        fprintf(ERRFP, "Error %d with utime() on %s: %s\n", errno, filename, strerror(errno));
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* Make directory <name> */
int make_dir(const char * name)
{
    struct stat st;
    if(stat(name, & st) != 0) /* Don't exist */
    {
        if(mkdir(name, MODE_DIR) != 0)
        {
            fprintf(ERRFP, "Error %d with mkdir(): %s\n", errno, strerror(errno));
            return EXIT_FAILURE;
        }
    }
    else if(!S_ISDIR(st.st_mode)) /* Not directory */
    {
        errno = ENOTDIR;
        fprintf(ERRFP, "Error %d with mkdir(): %s\n", errno, strerror(errno));
        return EXIT_FAILURE;
    }
    chmod(name, MODE_DIR); /* Change access permissions */
    return EXIT_SUCCESS;
}

/* Recursive make directory <path> */
int make_path(const char * path)
{
    int8_t flag = 1;
    int status = EXIT_SUCCESS;
    char tmppath[STRBUFSIZE];
    char * curr = tmppath;
    strncpy(tmppath, path, sizeof(tmppath) - 1);

    while(flag)
    {
        curr = strchr(curr, '/');
        if(curr)
        {
            * curr = '\0';
            status = make_dir(tmppath);
            * curr = '/';
            curr = curr + 1;
        }
        else if(status == EXIT_SUCCESS)
        {
            status = make_dir(tmppath);
            flag = 0;
        }
    }
    return status;
}

/* Recursive make directoty for file <filename> */
int make_path_for(char * filename)
{
    char * pp = strrchr(filename, '/');
    int status = EXIT_SUCCESS;
    if(pp)
    {
        * pp = '\0';
        status = make_path(filename);
        * pp = '/';
    }
    return status;
}

/* Delete files by mask <mask> in directory <directory> */
int delete_files(const char * directory, const char * mask)
{
    DIR * dfd = opendir(directory);
    struct dirent * dp;

    if(dfd == NULL)
    {
        fprintf(ERRFP, "Error: No such directory %s\n", directory);
        return EXIT_FAILURE;
    }

    while((dp = readdir(dfd)) != NULL)
    {
        const char * curr_name = dp->d_name;
        const char * curr_mask = mask;
        int8_t flag = 1;
        while(flag)
        {
            if(* curr_mask == '*')
            {
                while(* curr_mask == '*')
                    curr_mask++;
                if(* curr_mask == '\0')
                    flag = 0;
                else
                {
                    while(* curr_name != '\0' && * curr_name != * curr_mask)
                        curr_name++;
                    if(* curr_name == '\0')
                        break;
                    else
                    {
                        curr_name++;
                        curr_mask++;
                        if(* curr_name == '\0' && * curr_mask == '\0')
                            flag = 0;
                    }
                }
            }
            else if(* curr_mask == '?' || * curr_name == * curr_mask)
            {
                curr_name++;
                curr_mask++;
                if(* curr_name == '\0' && * curr_mask == '\0')
                    flag = 0;
            }
            else
                break;
        }
        if(!flag)
        {
            char buf[STRBUFSIZE];
            sprintf(buf, "%s/%s", directory, dp->d_name);
            if(remove(buf) != 0)
                fprintf(ERRFP, "Error: Can't delete file %s/%s\n", directory, dp->d_name);
        }
    }

    closedir(dfd);
    return EXIT_SUCCESS;
}

/* Check <filename> exist */
int exist(const char * filename)
{
    struct stat st;
    if(stat(filename, & st) != 0)
        return 0;
    return 1;
}

/* Get <filename> size */
off_t get_size(const char * filename)
{
    struct stat st;
    if(stat(filename, & st) != 0)
    {
        fprintf(ERRFP, "Error %d with stat(): %s\n", errno, strerror(errno));
        return -1;
    }
    return st.st_size;
}

/* Open temp file */
FILE * fopen_temp(char * filename)
{
    FILE * tmpf = tmpfile();
    if(tmpf == NULL && filename != NULL) /* In some strange cases, tmpfile() does not work */
    {
        tmpnam(filename);
        tmpf = fopen(filename, "wb+");

#if defined(__CYGWIN__)
        if(tmpf == NULL) /* In some strange cases with cygwin, tmpnam() return broken path */
        {
            char sb[L_tmpnam];
            char * nm = strrchr(filename, '/');
            if(nm != NULL)
                strncpy(sb, nm, sizeof(sb) - 1);
            else
                strncpy(sb, filename, sizeof(sb) - 1);
            sprintf(filename, "%s%s", getenv("TEMP"), sb);
            tmpf = fopen(filename, "wb+");

            if(tmpf == NULL) /* Hmm, maybe TMP instead of TEMP will work? */
            {
                if(nm != NULL)
                    strncpy(sb, nm, sizeof(sb) - 1);
                else
                    strncpy(sb, filename, sizeof(sb) - 1);
                sprintf(filename, "%s%s", getenv("TMP"), sb);
                tmpf = fopen(filename, "wb+");

                if(tmpf == NULL) /* So sad... */
                {
                    fprintf(ERRFP, "Error: Can't create temporary file.\n");
                    return NULL;
                }
            }
        }
#else
        if(tmpf == NULL) /* So sad... */
        {
            fprintf(ERRFP, "Error: Can't create temporary file.\n");
            return NULL;
        }
#endif
    }
    else if(filename != NULL)
        filename[0] = '\0';
    return tmpf;
}
