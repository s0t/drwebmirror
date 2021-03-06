/*
   Copyright (C) 2014-2019, Rudolf Sikorski <rudolf.sikorski@freenet.de>

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

/* UserID from license key */
char key_userid[33];
/* MD5 sum of license key */
char key_md5sum[33];

/* Tree for caching checksums in fast mode */
avl_node * tree;
/* Flag of use fast mode */
int8_t use_fast;

/* Get UserID and MD5 sum from keyfile */
int parse_keyfile(const char * filename)
{
    char str[255];
    FILE * fp = fopen(filename, "r");
    int8_t flag = 1;

    if(fp == NULL)
    {
        fprintf(ERRFP, "Error with fopen() on %s\n", filename);
        return EXIT_FAILURE;
    }

    while(flag) /* Find "[User]" block */
    {
        if(fscanf(fp, "%[^\r\n]\r\n", str) == -1)
        {
            fprintf(ERRFP, "Unexpected EOF on %s\n", filename);
            fclose(fp);
            return EXIT_FAILURE;
        }
        if(strcmp(str, "[User]") == 0)
            flag = 0;
    }

    flag = 1;
    while(flag) /* Find "Number" field */
    {
        if(fscanf(fp, "%[^\r\n]\r\n", str) == -1)
        {
            fprintf(ERRFP, "Unexpected EOF on %s\n", filename);
            fclose(fp);
            return EXIT_FAILURE;
        }
        if(strstr(str, "Number"))
            flag = 0;
    }

    fclose(fp);

    bsd_strlcpy(key_userid, strchr(str, '=') + 1, sizeof(key_userid));

    if(md5sum(filename, key_md5sum) != EXIT_SUCCESS) /* MD5 sum */
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

/* Build caching tree for v4 */
static void cache4(void)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag = 1;
    sprintf(buf, "%s/%s", remotedir, "drweb32.lst");
    fp = fopen(buf, "r");
    if(!fp) return;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(buf[0] == '+' || buf[0] == '=' || buf[0] == '!') /* Need to download this file */
        {
            char filename[STRBUFSIZE];
            char crc_base[9];
            char * beg = buf + 1, * tmp;
            tmp = strchr(beg, '>'); /* if some as "=<w95>spider.vxd, C54AAA37" */
            if(tmp) beg = tmp + 1;
            tmp = strrchr(beg, '\\'); /* if some as "=<wnt>%SYSDIR%\spider.cpl, 871D501E" */
            if(tmp) beg = tmp + 1;
            sprintf(filename, "%s/%s", remotedir, beg);
            * strchr(filename, ',') = '\0';
            tmp = strchr(filename, '|'); /* if some as "!drwreg.exe|-xi, FE7E4B36" */
            if(tmp) * tmp = '\0';
            tmp = strchr(buf, ',');
            do tmp++; while(* tmp == ' ');
            bsd_strlcpy(crc_base, tmp, sizeof(crc_base));
            while(crc_base[0] == '0') /* if base crc32 beign with zero */
                memmove(crc_base, crc_base + 1, sizeof(char) * strlen(crc_base));
            tree = avl_insert(tree, filename, crc_base);
            strcat(filename, ".lzma");
            tree = avl_insert(tree, filename, crc_base);
        }
    }
    fclose(fp);
}

/* Update using version 4 of update protocol (flat file drweb32.lst, crc32) */
int update4(void)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag;
    int counter_global = 0, status;
    char main_hash_old[65], main_hash_new[65];
    off_t main_size_old = 0, main_size_new = 0;

    if(make_path(remotedir) != EXIT_SUCCESS) /* Make all needed directory */
    {
        fprintf(ERRFP, "Error: Can't access to local directory\n");
        return EXIT_FAILURE;
    }
    if(do_lock(remotedir) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    if(use_fast)
    {
        sprintf(buf, "%s/%s", remotedir, "drweb32.lst");
        status = sha256sum(buf, main_hash_old);
        if(status != EXIT_SUCCESS)
        {
            use_fast = 0;
            fprintf(ERRFP, "Warning: drweb32.lst was not found\n");
            fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
        }
        else
        {
            main_size_old = get_size(buf);
            cache4();
        }
    }

repeat4: /* Goto here if checksum mismatch */
    if(counter_global > 0 && use_fast) /* Incomplete update will lead to integrity violations */
    {
        use_fast = 0;
        fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
    }

    sprintf(buf, "%s/%s", remotedir, "drweb32.lst");
    status = download(buf);
    if(!DL_SUCCESS(status))
        return EXIT_FAILURE;
    if(use_fast)
    {
        main_size_new = get_size(buf);
        if(main_size_new == main_size_old)
        {
            sha256sum(buf, main_hash_new);
            if(strcmp(main_hash_old, main_hash_new) == 0)
            {
                if(verbose)
                    printf("Nothing was changed\n");
                return EXIT_SUCCESS;
            }
        }
    }
    /* Optional files */
    sprintf(buf, "%s/%s", remotedir, "drweb32.lst.lzma");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "version.lst");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "version.lst.lzma");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "drweb32.flg");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "drweb32.flg.lzma");
    download(buf);

    /* Main file */
    sprintf(buf, "%s/%s", remotedir, "drweb32.lst");
    fp = fopen(buf, "r");
    flag = 1;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(buf[0] == '+' || buf[0] == '=' || buf[0] == '!') /* Need to download this file */
        {
            char filename[STRBUFSIZE];
            char crc_base[9], crc_real[9];
            char * beg = buf + 1, * tmp;
            tmp = strchr(beg, '>'); /* if some as "=<w95>spider.vxd, C54AAA37" */
            if(tmp) beg = tmp + 1;
            tmp = strrchr(beg, '\\'); /* if some as "=<wnt>%SYSDIR%\spider.cpl, 871D501E" */
            if(tmp) beg = tmp + 1;
            sprintf(filename, "%s/%s", remotedir, beg);
            * strchr(filename, ',') = '\0';
            tmp = strchr(filename, '|'); /* if some as "!drwreg.exe|-xi, FE7E4B36" */
            if(tmp) * tmp = '\0';
            tmp = strchr(buf, ',');
            do tmp++; while(* tmp == ' ');
            bsd_strlcpy(crc_base, tmp, sizeof(crc_base));
            while(crc_base[0] == '0') /* if base crc32 beign with zero */
                memmove(crc_base, crc_base + 1, sizeof(char) * strlen(crc_base));

            status = download_check(filename, crc_base, crc_real, & crc32sum, "CRC32");
            if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
            {
                counter_global++;
                fclose(fp);
                sleep(REPEAT_SLEEP);
                goto repeat4; /* Yes, it is goto. Sorry, Dijkstra... */
            }
            else if(!DL_SUCCESS(status))
            {
                fclose(fp);
                return EXIT_FAILURE;
            }

            sprintf(buf, "%s.lzma", filename); /* Also get lzma file, if exist */
            if(status == DL_DOWNLOADED || exist(buf))
            {
                status = download_check(buf, crc_base, crc_real, & crc32sum_lzma, "CRC32 LZMA");
                if(status == DL_NOT_FOUND) /* Need for delete lzma file */
                {
                    if(exist(buf))
                    {
                        char * nm = strrchr(buf, '/') + 1;
                        memmove(buf, nm, (strlen(nm) + 1) * sizeof(char));
                        printf("Deleting... %s\n", buf);
                        delete_files(remotedir, buf);
                    }
                }
                else if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
                {
                    counter_global++;
                    fclose(fp);
                    sleep(REPEAT_SLEEP);
                    goto repeat4; /* Yes, it is goto. Sorry, Dijkstra... */
                }
                else if(!DL_SUCCESS(status))
                {
                    fclose(fp);
                    return EXIT_FAILURE;
                }
            }
        }
        else if(buf[0] == '-') /* Need to delete this file */
        {
            char filename[STRBUFSIZE];
            sprintf(filename, "%s", buf + 1);
            * strchr(filename, ',') = '\0';
            delete_files(remotedir, filename);
            strcat(filename, ".lzma");
            delete_files(remotedir, filename);
        }
    }

    fclose(fp);
    return EXIT_SUCCESS;
}

/* Build caching tree for v5 */
static void cache5(const char * const version_file)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag = 1;
    sprintf(buf, "%s/%s", remotedir, version_file);
    fp = fopen(buf, "r");
    if(!fp) return;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(buf[0] == '+' || buf[0] == '=' || buf[0] == '!') /* Need to download this file */
        {
            char filename[STRBUFSIZE];
            char sha_base[65];
            char * beg = buf + 1, * tmp;
            tmp = strchr(beg, '>'); /* if some as "=<w95>spider.vxd, ..." */
            if(tmp) beg = tmp + 1;
            tmp = strrchr(beg, '\\'); /* if some as "=<wnt>%SYSDIR%\spider.cpl, ..." */
            if(tmp) beg = tmp + 1;
            sprintf(filename, "%s/%s", remotedir, beg);
            * strchr(filename, ',') = '\0';
            tmp = strchr(filename, '|'); /* if some as "!drwreg.exe|-xi, ..." */
            if(tmp) * tmp = '\0';
            tmp = strchr(buf, ',');
            do tmp++; while(* tmp == ' ');
            bsd_strlcpy(sha_base, tmp, sizeof(sha_base));
            tree = avl_insert(tree, filename, sha_base);
            strcat(filename, ".lzma");
            tree = avl_insert(tree, filename, sha_base);
        }
    }
    fclose(fp);
}

/* Update using version 5 or 5v2 of update protocol */
static int update5x_internal(const char * const version_file)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag;
    int counter_global = 0, status;
    char main_hash_old[65], main_hash_new[65];
    off_t main_size_old = 0, main_size_new = 0;

    if(make_path(remotedir) != EXIT_SUCCESS) /* Make all needed directory */
    {
        fprintf(ERRFP, "Error: Can't access to local directory\n");
        return EXIT_FAILURE;
    }
    if(do_lock(remotedir) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    if(use_fast)
    {
        sprintf(buf, "%s/%s", remotedir, version_file);
        status = sha256sum(buf, main_hash_old);
        if(status != EXIT_SUCCESS)
        {
            use_fast = 0;
            fprintf(ERRFP, "Warning: %s was not found\n", version_file);
            fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
        }
        else
        {
            main_size_old = get_size(buf);
            cache5(version_file);
        }
    }

repeat5: /* Goto here if checksum mismatch */
    if(counter_global > 0 && use_fast) /* Incomplete update will lead to integrity violations */
    {
        use_fast = 0;
        fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
    }

    sprintf(buf, "%s/%s", remotedir, version_file);
    status = download(buf);
    if(!DL_SUCCESS(status))
        return EXIT_FAILURE;
    if(use_fast)
    {
        main_size_new = get_size(buf);
        if(main_size_new == main_size_old)
        {
            sha256sum(buf, main_hash_new);
            if(strcmp(main_hash_old, main_hash_new) == 0)
            {
                if(verbose)
                    printf("Nothing was changed\n");
                return EXIT_SUCCESS;
            }
        }
    }
    /* Optional files */
    sprintf(buf, "%s/%s.lzma", remotedir, version_file);
    download(buf);
    /* Usually, these files can be downloaded with version.lst */
    /* Uncomment lines below if something wrong */
    /*
    sprintf(buf, "%s/%s", remotedir, "drweb32.lst");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "drweb32.lst.lzma");
    download(buf);
    */
    sprintf(buf, "%s/%s", remotedir, "drweb32.flg");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "drweb32.flg.lzma");
    download(buf);
    if(strcmp(version_file, "version.lst") != 0)
    {
        sprintf(buf, "%s/%s", remotedir, "version.lst");
        download(buf);
        sprintf(buf, "%s/%s", remotedir, "version.lst.lzma");
        download(buf);
    }

    /* Main file */
    sprintf(buf, "%s/%s", remotedir, version_file);
    fp = fopen(buf, "r");
    flag = 1;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(buf[0] == '+' || buf[0] == '=' || buf[0] == '!') /* Need to download this file */
        {
            char filename[STRBUFSIZE];
            char sha_base[65], sha_real[65], sha_lzma_base[65], sha_lzma_real[65];
            off_t filesize = -1, filesize_lzma = -1;
            char * beg = buf + 1, * tmp;
            int8_t has_sha_lzma = 0;
            tmp = strchr(beg, '>'); /* if some as "=<w95>spider.vxd, ..." */
            if(tmp) beg = tmp + 1;
            tmp = strrchr(beg, '\\'); /* if some as "=<wnt>%SYSDIR%\spider.cpl, ..." */
            if(tmp) beg = tmp + 1;
            sprintf(filename, "%s/%s", remotedir, beg);
            * strchr(filename, ',') = '\0';
            tmp = strchr(filename, '|'); /* if some as "!drwreg.exe|-xi, ..." */
            if(tmp) * tmp = '\0';
            tmp = strchr(buf, ',');
            do tmp++; while(* tmp == ' ');
            bsd_strlcpy(sha_base, tmp, sizeof(sha_base));
            tmp += sizeof(sha_base) - 1;
            tmp = strchr(tmp, ',');
            if(tmp)
            {
                unsigned long filesize_ul = 0;
                tmp++;
                sscanf(tmp, "%lu", & filesize_ul);
                filesize = (off_t)filesize_ul;

                /* optional LZMA SHA256 + LZMA size */
                tmp = strchr(tmp, ',');
                if(tmp)
                {
                    do tmp++; while(* tmp == ' ');
                    has_sha_lzma = 1;
                    bsd_strlcpy(sha_lzma_base, tmp, sizeof(sha_lzma_base));
                    tmp += sizeof(sha_lzma_base) - 1;

                    tmp = strchr(tmp, ',');
                    if(tmp)
                    {
                        filesize_ul = 0;
                        tmp++;
                        sscanf(tmp, "%lu", & filesize_ul);
                        filesize_lzma = (off_t)filesize_ul;
                    }
                }
            }

            status = download_check(filename, sha_base, sha_real, & sha256sum, "SHA256");
            if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
            {
                counter_global++;
                fclose(fp);
                sleep(REPEAT_SLEEP);
                goto repeat5; /* Yes, it is goto. Sorry, Dijkstra... */
            }
            else if(!DL_SUCCESS(status))
            {
                fclose(fp);
                return EXIT_FAILURE;
            }
            if(filesize >= 0 && !check_size(filename, filesize)) /* Wrong size */
            {
                fclose(fp);
                if(counter_global >= MAX_REPEAT)
                    return EXIT_FAILURE;
                counter_global++;
                sleep(REPEAT_SLEEP);
                goto repeat5; /* Yes, it is goto. Sorry, Dijkstra... */
            }

            sprintf(buf, "%s.lzma", filename); /* Also get lzma file, if exist */
            if(status == DL_DOWNLOADED || exist(buf))
            {
                status = download_check(buf, sha_base, sha_real, & sha256sum_lzma, "SHA256 LZMA");
                if(status == DL_NOT_FOUND) /* Need for delete lzma file */
                {
                    if(exist(buf))
                    {
                        char * nm = strrchr(buf, '/') + 1;
                        memmove(buf, nm, (strlen(nm) + 1) * sizeof(char));
                        printf("Deleting... %s\n", buf);
                        delete_files(remotedir, buf);
                    }
                }
                else if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
                {
                    counter_global++;
                    fclose(fp);
                    sleep(REPEAT_SLEEP);
                    goto repeat5; /* Yes, it is goto. Sorry, Dijkstra... */
                }
                else if(!DL_SUCCESS(status))
                {
                    fclose(fp);
                    return EXIT_FAILURE;
                }
                else if((filesize >= 0 && !check_size_lzma(buf, filesize)) ||
                        (filesize_lzma >= 0 && !check_size(buf, filesize_lzma))) /* Wrong size */
                {
                    fclose(fp);
                    if(counter_global >= MAX_REPEAT)
                        return EXIT_FAILURE;
                    counter_global++;
                    sleep(REPEAT_SLEEP);
                    goto repeat5; /* Yes, it is goto. Sorry, Dijkstra... */
                }
                else if(!use_fast && has_sha_lzma)
                {
                    if(verbose)
                        printf("%s %s, checking SHA256 ", buf, (status == DL_EXIST ? "exist" : "downloaded"));
                    if(sha256sum(buf, sha_lzma_real) != EXIT_SUCCESS || strcmp(sha_lzma_base, sha_lzma_real) != 0) /* Sum mismatched */
                    {
                        if(verbose)
                            printf("[NOT OK]\n");

                        fprintf(ERRFP, "Warning: SHA256 mismatch (real=\"%s\", base=\"%s\")\n", sha_lzma_real, sha_lzma_base);

                        fclose(fp);
                        if(counter_global >= MAX_REPEAT)
                            return EXIT_FAILURE;
                        counter_global++;
                        sleep(REPEAT_SLEEP);
                        goto repeat5; /* Yes, it is goto. Sorry, Dijkstra... */
                    }
                    else
                    {
                        if(verbose)
                            printf("[OK]\n");
                    }
                }
            }
        }
        else if(buf[0] == '-') /* Need to delete this file */
        {
            char filename[STRBUFSIZE];
            sprintf(filename, "%s", buf + 1);
            * strchr(filename, ',') = '\0';
            delete_files(remotedir, filename);
            strcat(filename, ".lzma");
            delete_files(remotedir, filename);
        }
    }

    fclose(fp);
    return EXIT_SUCCESS;
}

/* Update using version 5 of update protocol (flat file version.lst, sha256) */
int update5(void)
{
    return update5x_internal("version.lst");
}

/* Update using version 5.2 of update protocol (flat file version2.lst, sha256) */
int update52(void)
{
    return update5x_internal("version2.lst");
}

/* Build caching tree for v7 */
static void cache7(const char * file, const char * directory)
{
    char buf[STRBUFSIZE];
    FILE * fp = fopen(file, "r");
    int8_t flag = 1;
    if(!fp) return;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(strstr(buf, "<xml") != NULL || strstr(buf, "<lzma") != NULL) /* file description found */
        {
            char base_hash[65];
            char filename[STRBUFSIZE];
            bsd_strlcpy(base_hash, strstr(buf, "hash=\"") + 6, sizeof(base_hash));
            sprintf(filename, "%s/%s", directory, strstr(buf, "name=\"") + 6);
            * strchr(filename, '\"') = '\0';
            tree = avl_insert(tree, filename, base_hash);
        }
    }
    fclose(fp);
}

/* Update using version 7 of update protocol (xml files, sha256) */
int update7(void)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag;
    int counter_global = 0;
    int status;
    char main_hash_old[65], main_hash_new[65];
    off_t main_size_old = 0, main_size_new = 0;

    if(make_path(remotedir) != EXIT_SUCCESS)
    {
        fprintf(ERRFP, "Error: Can't access to local directory\n");
        return EXIT_FAILURE;
    }
    if(do_lock(remotedir) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    if(use_fast)
    {
        sprintf(buf, "%s/%s", remotedir, "versions.xml");
        status = sha256sum(buf, main_hash_old);
        if(status != EXIT_SUCCESS)
        {
            use_fast = 0;
            fprintf(ERRFP, "Warning: versions.xml was not found\n");
            fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
        }
        else
        {
            main_size_old = get_size(buf);
            cache7(buf, remotedir);
        }
    }

repeat7: /* Goto here if hashsum mismatch */
    if(counter_global > 0 && use_fast) /* Incomplete update will lead to integrity violations */
    {
        use_fast = 0;
        fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
    }

    /* Optional files (WTF???)*/
    /* Uncomment lines below if something wrong */
    /*
    sprintf(buf, "%s/%s", remotedir, "repodb.xml");
    download(buf);
    sprintf(buf, "%s/%s", remotedir, "revisions.xml");
    download(buf);
    */

    /* Get versions.xml */
    sprintf(buf, "%s/%s", remotedir, "versions.xml");
    status = download(buf);
    if(!DL_SUCCESS(status))
        return EXIT_FAILURE;
    if(use_fast)
    {
        main_size_new = get_size(buf);
        if(main_size_new == main_size_old)
        {
            sha256sum(buf, main_hash_new);
            if(strcmp(main_hash_old, main_hash_new) == 0)
            {
                if(verbose)
                    printf("Nothing was changed\n");
                return EXIT_SUCCESS;
            }
        }
    }

    /* Parse versions.xml */
    fp = fopen(buf, "r");
    flag = 1;
    while(flag)
    {
        if(fscanf(fp, "%[^\r\n]\r\n", buf) == -1)
            flag = 0;
        else if(strstr(buf, "<xml") != NULL || strstr(buf, "<lzma") != NULL) /* file description found */
        {
            char base_hash[65];
            char real_hash[65];
            char filename[STRBUFSIZE];
            int8_t is_xml = 0;
            off_t filesize = -1;
            unsigned long filesize_ul = 0;
            char * tmpchr;

            if(strstr(buf, "<xml") != NULL) is_xml = 1;

            bsd_strlcpy(base_hash, strstr(buf, "hash=\"") + 6, sizeof(base_hash));
            sprintf(filename, "%s/%s", remotedir, strstr(buf, "name=\"") + 6);
            * strchr(filename, '\"') = '\0';
            if((tmpchr = strstr(buf, "size=\"")) != NULL)
            {
                sscanf(tmpchr + 6, "%lu\"", & filesize_ul);
                filesize = (off_t)filesize_ul;
            }

            if(!exist(filename) && make_path_for(filename) != EXIT_SUCCESS) /* If file not exist, check directories and make it if need */
            {
                fprintf(ERRFP, "Error: Can't access to local directory\n");
                fclose(fp);
                return EXIT_FAILURE;
            }
            else if(tree && counter_global == 0 && is_xml)
            {
                char directory[STRBUFSIZE];
                bsd_strlcpy(directory, filename, sizeof(directory));
                * strrchr(directory, '/') = '\0';
                cache7(filename, directory);
            }

            status = download_check(filename, base_hash, real_hash, & sha256sum, "SHA256");
            if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
            {
                counter_global++;
                fclose(fp);
                sleep(REPEAT_SLEEP);
                goto repeat7; /* Yes, it is goto. Sorry, Dijkstra... */
            }
            else if(!DL_SUCCESS(status))
            {
                fclose(fp);
                return EXIT_FAILURE;
            }
            if(filesize >= 0 && !check_size(filename, filesize)) /* Wrong size */
            {
                fclose(fp);
                if(counter_global >= MAX_REPEAT)
                    return EXIT_FAILURE;
                counter_global++;
                sleep(REPEAT_SLEEP);
                goto repeat7; /* Yes, it is goto. Sorry, Dijkstra... */
            }

            if(is_xml) /* Parse this xml file */
            {
                char directory[STRBUFSIZE];
                FILE * xfp = fopen(filename, "r");
                int8_t xflag = 1;
                char * pp = strrchr(filename, '/');
                * pp = '\0';
                bsd_strlcpy(directory, filename, sizeof(directory));
                * pp = '/';

                while(xflag)
                {
                    if(fscanf(xfp, "%[^\r\n]\r\n", buf) == -1)
                        xflag = 0;
                    else if(strstr(buf, "<lzma") != NULL) /* lzma file description found */
                    {
                        char xfilename[STRBUFSIZE];
                        int status;
                        off_t xfilesize = -1;
                        unsigned long xfilesize_ul = 0;

                        bsd_strlcpy(base_hash, strstr(buf, "hash=\"") + 6, sizeof(base_hash));
                        sprintf(xfilename, "%s/%s", directory, strstr(buf, "name=\"") + 6);
                        * strchr(xfilename, '\"') = '\0';
                        if((tmpchr = strstr(buf, "size=\"")) != NULL)
                        {
                            sscanf(tmpchr + 6, "%lu\"", & xfilesize_ul);
                            xfilesize = (off_t)xfilesize_ul;
                        }

                        if(!exist(xfilename) && make_path_for(xfilename) != EXIT_SUCCESS) /* If file not exist, check directories and make it if need */
                        {
                            fprintf(ERRFP, "Error: Can't access to local directory\n");
                            fclose(fp);
                            fclose(xfp);
                            return EXIT_FAILURE;
                        }

                        status = download_check(xfilename, base_hash, real_hash, & sha256sum, "SHA256");
                        if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
                        {
                            counter_global++;
                            fclose(fp);
                            fclose(xfp);
                            sleep(REPEAT_SLEEP);
                            goto repeat7; /* Yes, it is goto. Sorry, Dijkstra... */
                        }
                        else if(!DL_SUCCESS(status))
                        {
                            fclose(fp);
                            fclose(xfp);
                            return EXIT_FAILURE;
                        }
                        if(xfilesize >= 0 && !check_size(xfilename, xfilesize)) /* Wrong size */
                        {
                            fclose(fp);
                            fclose(xfp);
                            if(counter_global >= MAX_REPEAT)
                                return EXIT_FAILURE;
                            counter_global++;
                            sleep(REPEAT_SLEEP);
                            goto repeat7; /* Yes, it is goto. Sorry, Dijkstra... */
                        }
                    }
                }
                fclose(xfp);
            }
        }
    }

    fclose(fp);
    return EXIT_SUCCESS;
}

/* Build caching tree for Android */
static void cacheA(const char * directory)
{
    char buf[STRBUFSIZE];
    FILE * fp;
    int8_t flag = 1, flag_files = 0;
    fp = fopen(remotedir, "r");
    if(!fp) return;
    while(flag)
    {
        if(fscanf(fp, "%[^\n]\n", buf) == -1)
            flag = 0;
        else if(flag_files)
        {
            if(buf[0] == '[' || strlen(buf) < 84)
                flag = 0;
            else
            {
                char md5_base[33];
                char filename_base[STRBUFSIZE], filename[STRBUFSIZE];
                unsigned long file_op = 0;
                sscanf(buf, "%*[^,], %lx, %*[^,], %[^,], %*[^,], %*[^,], %s",
                       & file_op, md5_base, filename_base);
                if(file_op == 0x0) /* Need to download this file */
                {
                    sprintf(filename, "%s/%s", directory, filename_base);
                    to_lowercase(md5_base);
                    tree = avl_insert(tree, filename, md5_base);
                }
            }
        }
        else
        {
            if(strncmp(buf, "[Files]", 7) == 0)
                flag_files = 1;
        }
    }
    fclose(fp);
}

/* Update using Android update protocol (flat file for mobile devices) */
int updateA(void)
{
    char buf[STRBUFSIZE], real_dir[STRBUFSIZE];
    FILE * fp;
    int8_t flag, flag_files;
    int counter_global = 0;
    int status;
    char main_hash_old[65], main_hash_new[65];
    off_t main_size_old = 0, main_size_new = 0;

    bsd_strlcpy(real_dir, remotedir, sizeof(real_dir));
    * (strrchr(real_dir, '/')) = '\0';
    if(make_path(real_dir) != EXIT_SUCCESS) /* Make all needed directory */
    {
        fprintf(ERRFP, "Error: Can't access to local directory\n");
        return EXIT_FAILURE;
    }
    if(do_lock(real_dir) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    if(use_fast)
    {
        status = sha256sum(remotedir, main_hash_old);
        if(status != EXIT_SUCCESS)
        {
            char * name = strrchr(remotedir, '/');
            if(name)
                name++;
            else
                name = remotedir;
            use_fast = 0;
            fprintf(ERRFP, "Warning: %s was not found\n", name);
            fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
        }
        else
        {
            main_size_old = get_size(remotedir);
            cacheA(real_dir);
        }
    }

repeatA: /* Goto here if checksum mismatch */
    if(counter_global > 0 && use_fast) /* Incomplete update will lead to integrity violations */
    {
        use_fast = 0;
        fprintf(ERRFP, "Warning: Fast mode has been disabled\n");
    }

    status = download(remotedir);
    if(!DL_SUCCESS(status))
        return EXIT_FAILURE;
    if(use_fast)
    {
        main_size_new = get_size(remotedir);
        if(main_size_new == main_size_old)
        {
            sha256sum(remotedir, main_hash_new);
            if(strcmp(main_hash_old, main_hash_new) == 0)
            {
                if(verbose)
                    printf("Nothing was changed\n");
                return EXIT_SUCCESS;
            }
        }
    }
    fp = fopen(remotedir, "r");
    flag = 1;
    flag_files = 0;
    while(flag)
    {
        if(fscanf(fp, "%[^\n]\n", buf) == -1)
            flag = 0;
        else if(flag_files)
        {
            if(buf[0] == '[' || strlen(buf) < 84)
                flag = 0;
            else
            {
                char md5_base[33], md5_real[33];
                char filename_base[STRBUFSIZE], filename[STRBUFSIZE];
                off_t filesize;
                unsigned long filesize_ul = 0;
                unsigned long file_op = 0;

                sscanf(buf, "%*[^,], %lx, %lx, %[^,], %*[^,], %*[^,], %s",
                       & file_op, & filesize_ul, md5_base, filename_base);
                sprintf(filename, "%s/%s", real_dir, filename_base);
                to_lowercase(md5_base);
                filesize = (off_t)filesize_ul;

                switch(file_op)
                {
                case 0x0: /* Need to download this file */
                {
                    int status = download_check(filename, md5_base, md5_real, & md5sum, "MD5");
                    if(status == DL_TRY_AGAIN && counter_global < MAX_REPEAT) /* Try again */
                    {
                        counter_global++;
                        fclose(fp);
                        sleep(REPEAT_SLEEP);
                        goto repeatA; /* Yes, it is goto. Sorry, Dijkstra... */
                    }
                    else if(!DL_SUCCESS(status))
                    {
                        fclose(fp);
                        return EXIT_FAILURE;
                    }
                    if(!check_size(filename, filesize)) /* Wrong size */
                    {
                        fclose(fp);
                        if(counter_global >= MAX_REPEAT)
                            return EXIT_FAILURE;
                        counter_global++;
                        sleep(REPEAT_SLEEP);
                        goto repeatA; /* Yes, it is goto. Sorry, Dijkstra... */
                    }
                    break;
                }
                case 0x2: /* Need to delete this file */
                {
                    if(exist(filename))
                    {
                        printf("Deleting %s\n", filename);
                        delete_files(real_dir, filename_base);
                    }
                    break;
                }
                default:
                {
                    fprintf(ERRFP, "Error: Unknown file operation %08lx for fine %s\n", file_op, filename_base);
                    fclose(fp);
                    return EXIT_FAILURE;
                }
                }
            }
        }
        else
        {
            if(strncmp(buf, "[Files]", 7) == 0)
                flag_files = 1;
        }
    }

    fclose(fp);
    return EXIT_SUCCESS;
}
