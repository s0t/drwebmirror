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
#include <sys/types.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#if !defined (NO_POSIX_API)
#include <sys/time.h>
#else
typedef int ssize_t;
#endif

#if defined (_WIN32)
#include <winsock.h>
#include <windows.h>
#define send(s, buf, len, flags) (send((s), (buf), (int)(len), (flags)))
#define recv(s, buf, len, flags) (recv((s), (buf), (int)(len), (flags)))
#define SOCKET_BAD_VALUE (INVALID_SOCKET)
typedef int socklen_t;
typedef SOCKET sockfd_t;
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#define SOCKET_BAD_VALUE (-1)
typedef int sockfd_t;
#endif

extern char * strptime(const char * buf, const char * format, struct tm * tm);
extern const char * hstrerror(int err);

/* Server name */
char servername[256];
/* Server port */
uint16_t serverport;
/* HTTP version */
char http_version[4];
/* Server auth */
int8_t use_http_auth;
char http_auth[77];
/* Remote directory */
char remotedir[256];
/* User Agent */
char useragent[256];
/* SysHash */
char syshash[33];
int8_t use_syshash;
/* Android */
int8_t use_android;
/* Proxy parameters */
int8_t use_proxy;
int8_t use_proxy_auth;
char proxy_address[256];
uint16_t proxy_port;
char proxy_auth[77];
/* Keep-Alive connection descriptor */
static sockfd_t sock_fd_ka;

/* Check socket status */
static int socket_good(sockfd_t * sock_fd)
{
#if defined(_WIN32)
    return * sock_fd != INVALID_SOCKET;
#else
    return * sock_fd >= 0;
#endif
}

/* Close connection */
static void conn_close(sockfd_t * sock_fd)
{
#if defined(_WIN32)
    closesocket(* sock_fd);
#else
    shutdown(* sock_fd, SHUT_RDWR);
    close(* sock_fd);
#endif
    sock_fd_ka = SOCKET_BAD_VALUE;
}

/* Startup network */
void conn_startup(void)
{
#if defined(_WIN32)
    WSADATA wsa_data;
    WORD wsa_ver = MAKEWORD(1, 1);
    memset(& wsa_data, 0, sizeof(WSADATA));
    WSAStartup(wsa_ver, & wsa_data);
#endif
    sock_fd_ka = SOCKET_BAD_VALUE;
}

/* Cleanup network */
void conn_cleanup(void)
{
    if(socket_good(&sock_fd_ka))
        conn_close(&sock_fd_ka);
#if defined(_WIN32)
    WSACleanup();
#endif
    sock_fd_ka = SOCKET_BAD_VALUE;
}

/* Open connection */
static int conn_open(sockfd_t * sock_fd, const char * server, uint16_t port)
{
    /* nginx/1.6.2: 85.10.234.30 */
    /* openresty/1.13.6.1: 46.46.160.202 */
    struct sockaddr_in sock_addr;
    struct hostent * host_info = gethostbyname(server);
    struct timeval tv;
    fd_set fdset;
#if defined(_WIN32)
    u_long sock_mode;
    DWORD sendrecv_timeout = TIMEOUT * 1000;
#else
    int sock_opts;
#endif

    if(host_info == NULL)
    {
#if defined(_WIN32)
        char * wsa_error_str = NULL;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                       NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
        fprintf(ERRFP, "Error %d with gethostbyname(): %s", WSAGetLastError(), wsa_error_str);
        LocalFree(wsa_error_str);
#else
        fprintf(ERRFP, "Error %d with gethostbyname(): %s\n", h_errno, hstrerror(h_errno));
#endif
        return EXIT_FAILURE;
    }

    * sock_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(!socket_good(sock_fd))
    {
#if defined(_WIN32)
        char * wsa_error_str = NULL;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                       NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
        fprintf(ERRFP, "Error %d with socket(): %s", WSAGetLastError(), wsa_error_str);
        LocalFree(wsa_error_str);
#else
        fprintf(ERRFP, "Error %d with socket(): %s\n", errno, strerror(errno));
#endif
        return EXIT_FAILURE;
    }

    /* Change to non-blocking mode */
#if defined(_WIN32)
    sock_mode = 1;
    ioctlsocket(* sock_fd, FIONBIO, & sock_mode);
#else
    sock_opts = fcntl(* sock_fd, F_GETFL);
    sock_opts |= O_NONBLOCK;
    fcntl(* sock_fd, F_SETFL, sock_opts);
#endif

    memset(& sock_addr, 0, sizeof(sock_addr));
    sock_addr.sin_family = host_info->h_addrtype;
    sock_addr.sin_port = htons(port);
    memcpy(& sock_addr.sin_addr.s_addr, host_info->h_addr_list[0], host_info->h_length);

    if(more_verbose)
    {
        struct in_addr addr;
        memcpy(& addr, host_info->h_addr_list[0], host_info->h_length);
        printf("Connection info:\n * Server Name: %s\n * Port: %d\n * IP: %s\n", server, (int)port, inet_ntoa(addr));
    }

#if defined(_WIN32)
    if(connect(* sock_fd, (const struct sockaddr *)(& sock_addr), sizeof(sock_addr)) != 0 &&
            WSAGetLastError() != WSAEINPROGRESS && WSAGetLastError() != WSAEWOULDBLOCK)
    {
        char * wsa_error_str = NULL;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                       NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
        fprintf(ERRFP, "Error %d with connect(): %s", WSAGetLastError(), wsa_error_str);
        LocalFree(wsa_error_str);
        conn_close(sock_fd);
        return EXIT_FAILURE;
    }
#else
    if(connect(* sock_fd, (const struct sockaddr *)(& sock_addr), sizeof(sock_addr)) != 0 &&
            errno != EINPROGRESS && errno != EWOULDBLOCK)
    {
        fprintf(ERRFP, "Error %d with connect(): %s\n", errno, strerror(errno));
        conn_close(sock_fd);
        return EXIT_FAILURE;
    }
#endif

    /* Set timeout value */
    memset(& tv, 0, sizeof(struct timeval));
    tv.tv_sec = TIMEOUT;
    FD_ZERO(& fdset);
    FD_SET(* sock_fd, & fdset);

    if(select((int)((* sock_fd) + 1), NULL, & fdset, NULL, & tv) == 1)
    {
        int so_error;
        socklen_t len = sizeof(so_error);
#if defined(_WIN32)
        getsockopt(* sock_fd, SOL_SOCKET, SO_ERROR, (char *)(& so_error), & len);
#else
        getsockopt(* sock_fd, SOL_SOCKET, SO_ERROR, & so_error, & len);
#endif

        if(so_error != 0)
        {
#if defined(_WIN32)
            char * wsa_error_str = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
            fprintf(ERRFP, "Error %d with select(): %s", WSAGetLastError(), wsa_error_str);
            LocalFree(wsa_error_str);
#else
            fprintf(ERRFP, "Error %d with select(): %s\n", so_error, strerror(so_error));
#endif
            conn_close(sock_fd);
            return EXIT_FAILURE;
        }
    }
    else
    {
        fprintf(ERRFP, "Error with select(): Connection timeout\n");
        conn_close(sock_fd);
        return EXIT_FAILURE;
    }

#if defined(_WIN32)
    /* Change to blocking mode */
    sock_mode = 0;
    ioctlsocket(* sock_fd, FIONBIO, & sock_mode);

    /* Set recv timeout value */
    if(setsockopt(* sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)(& sendrecv_timeout), sizeof(DWORD)) != 0)
        fprintf(ERRFP, "Warning: Can't set recv() timeout\n");
    /* Set send timeout value */
    if(setsockopt(* sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)(& sendrecv_timeout), sizeof(DWORD)) != 0)
        fprintf(ERRFP, "Warning: Can't set recv() timeout\n");
#else
    /* Change to blocking mode */
    sock_opts = fcntl(* sock_fd, F_GETFL);
    sock_opts ^= O_NONBLOCK;
    fcntl(* sock_fd, F_SETFL, sock_opts);

    /* Set recv timeout value */
    if(setsockopt(* sock_fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)(& tv), sizeof(struct timeval)) != 0)
        fprintf(ERRFP, "Warning: Can't set recv() timeout\n");
    /* Set send timeout value */
    if(setsockopt(* sock_fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)(& tv), sizeof(struct timeval)) != 0)
        fprintf(ERRFP, "Warning: Can't set recv() timeout\n");
#endif

    return EXIT_SUCCESS;
}

/* Get file <filename> from server */
static int conn_get(const char * filename)
{
    sockfd_t sock_fd;

    char * buffer, * bufpos, * bufend;
    unsigned long msgsize = 0;
    int status;
    int8_t msgbegin;
    unsigned long msgcurr = 0;
    size_t redirect_num = 0;
    size_t send_count, request_len;
    int8_t is_chunked = 0;

    time_t lastmod = 0;
    FILE * fp;
    char filename_dl[STRBUFSIZE];
    char servername_dl[256];
    uint16_t serverport_dl = serverport;
    char conn_ka[11];

    buffer = (char *)calloc(NETBUFSIZE + 4, sizeof(char));

    bsd_strlcpy(filename_dl, filename, sizeof(filename_dl));
    bsd_strlcpy(servername_dl, servername, sizeof(servername_dl));
    bsd_strlcpy(conn_ka, "Keep-Alive", sizeof(conn_ka));

    printf("Downloading %s\n", filename);

redirect: /* Goto here if 30x received */
    msgsize = 0;
    status = -1;
    msgbegin = 0;
    is_chunked = 0;
    lastmod = 0;

    if(strcmp(servername, servername_dl) != 0 || serverport != serverport_dl)
        bsd_strlcpy(conn_ka, "close", sizeof(conn_ka));

    if(use_proxy == 1)
    {
        if(!socket_good(&sock_fd_ka))
        {
            if(conn_open(&sock_fd, proxy_address, proxy_port) != EXIT_SUCCESS) /* Open connection */
            {
                free(buffer);
                return EXIT_FAILURE;
            }
        }
        else
        {
            sock_fd = sock_fd_ka;
        }

        sprintf(buffer,
                "GET http://%s:%u/%s HTTP/%s\r\n"
                "Proxy-Connection: %s\r\n",
                servername_dl, (unsigned)serverport_dl, filename_dl, http_version, conn_ka);
        if(use_proxy_auth == 1)
            sprintf(buffer + strlen(buffer),
                    "Proxy-Authorization: Basic %s\r\n",
                    proxy_auth);

    }
    else
    {
        if(!socket_good(&sock_fd_ka))
        {
            if(conn_open(&sock_fd, servername_dl, serverport_dl) != EXIT_SUCCESS) /* Open connection */
            {
                free(buffer);
                return EXIT_FAILURE;
            }
        }
        else
        {
            sock_fd = sock_fd_ka;
        }

        sprintf(buffer, "GET /%s HTTP/%s\r\n", filename_dl, http_version);
    }

    sprintf(buffer + strlen(buffer),
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Accept-Ranges: bytes\r\n"
            "Host: %s:%u\r\n",
            servername_dl, (unsigned)serverport_dl);
    if(use_http_auth == 1)
        sprintf(buffer + strlen(buffer),
                "Authorization: Basic %s\r\n",
                http_auth);
    if(use_android == 0)
        sprintf(buffer + strlen(buffer),
                "X-DrWeb-Validate: %s\r\n"
                "X-DrWeb-KeyNumber: %s\r\n",
                key_md5sum, key_userid);
    if(use_syshash == 1)
        sprintf(buffer + strlen(buffer),
                "X-DrWeb-SysHash: %s\r\n",
                syshash);
    if(useragent[0] != '\0')
        sprintf(buffer + strlen(buffer),
                "User-Agent: %s\r\n",
                useragent);
    sprintf(buffer + strlen(buffer),
            "Connection: %s\r\n"
            "Cache-Control: no-cache\r\n\r\n",
            conn_ka);

    request_len = strlen(buffer);
    if(more_verbose)
    {
        size_t i;
        printf("\n");
        for(i = 0; i < request_len; i++)
            if(buffer[i] != '\r')
                printf("%c", buffer[i]);
    }

    /* Number of bytes actually sent out might be less than the number you told it to send */
    /* See http://beej.us/guide/bgnet/output/html/multipage/syscalls.html#sendrecv for details */
    bufpos = bufend = buffer;
    send_count = 0;
    while(send_count < request_len)
    {
        ssize_t bytes_sent = send(sock_fd, bufpos, strlen(bufpos), 0); /* Send request */
        if(bytes_sent < 0)
        {
#if defined(_WIN32)
            char * wsa_error_str = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
            fprintf(ERRFP, "Error %d with send(): %s", WSAGetLastError(), wsa_error_str);
            LocalFree(wsa_error_str);
#else
            fprintf(ERRFP, "Error %d with send(): %s\n", errno, strerror(errno));
#endif
            conn_close(&sock_fd);
            free(buffer);
            return EXIT_FAILURE;
        }
        send_count += bytes_sent;
        bufpos += bytes_sent;
    }

    bufpos = buffer;
    buffer[0] = '\0';
    while(!msgbegin) /* Parse header of response */
    {
        char * tmp;
        char field_name[255];
        char field_content[STRBUFSIZE];
        size_t old_buf = (size_t)bufend - (size_t)buffer;
        ssize_t recv_count;

        bufpos = bufend;
        recv_count = recv(sock_fd, bufpos, NETBUFSIZE - old_buf, 0);
        bufend = bufpos + recv_count;
        * bufend = '\0';
        if(recv_count <= 0)
        {
#if defined(_WIN32)
            char * wsa_error_str = NULL;
            FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                           NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
            fprintf(ERRFP, "Error %d with recv(): %s", WSAGetLastError(), wsa_error_str);
            LocalFree(wsa_error_str);
#else
            fprintf(ERRFP, "Error %d with recv(): %s\n", errno, strerror(errno));
#endif
            conn_close(&sock_fd);
            free(buffer);
            return EXIT_FAILURE;
        }

        if(more_verbose)
        {
            char * smth;
            for(smth = bufpos; smth < bufend; smth++)
                if(* smth != '\r')
                    printf("%c", * smth);
                else if(smth + 2 < bufend && * (smth + 2) == '\r')
                {
                    printf("\n\n");
                    break;
                }
        }

        if(status < 0)
        {
            bufpos = strchr(buffer, ' ');
            if(bufpos != NULL)
            {
                bufpos++;
                sscanf(bufpos, "%d", & status);
                bufpos = strchr(bufpos, '\n');
                if(bufpos != NULL)
                    bufpos++;
                else
                    status = EXIT_FAILURE;
            }
            else
                status = EXIT_FAILURE;
            if(status == EXIT_FAILURE)
            {
                fprintf(ERRFP, "Error with recv(): Can't parse response\n");
                if(!socket_good(&sock_fd_ka))
                    conn_close(&sock_fd);
                free(buffer);
                return status;
            }

            /* Redirect */
            /* Warning: 300 work only if server set Location field */
            if(((status >= 300 && status <= 303) || status == 307) && redirect_num < MAX_REDIRECT)
            {
                redirect_num++;
                while((tmp = strchr(bufpos, '\r')) != NULL && tmp - 2 != strstr(bufpos - 2, "\r\n\r\n"))
                {
                    sscanf(bufpos, "%[^:]", field_name);
                    bufpos += strlen(field_name) + 2;
                    sscanf(bufpos, "%[^\r]", field_content);
                    bufpos += strlen(field_content) + 2;
                    if(strcmp(field_name, "Location") == 0) /* Parse new location */
                    {
                        char * servername_beg, * serverport_beg, * filename_beg;
                        servername_beg = strstr(field_content, "://");
                        if(servername_beg)
                        {
                            servername_beg += 3;
                            serverport_beg = strchr(servername_beg, ':');
                            filename_beg = strchr(servername_beg, '/');

                            if(* (filename_beg + 1) != '\0')
                                bsd_strlcpy(filename_dl, filename_beg + 1, sizeof(filename_dl));
                            else
                                strcpy(filename_dl, "/");
                            * filename_beg = '\0';
                            if(serverport_beg && serverport_beg < filename_beg) /* Non-default port */
                            {
                                serverport_dl = atoi(serverport_beg + 1);
                                * serverport_beg = '\0';
                            }
                            else
                                serverport_dl = 80;
                            bsd_strlcpy(servername_dl, servername_beg, sizeof(servername_dl));
                        }
                    }
                }
                if(verbose)
                    printf("Redirected (%d) to http://%s:%u/%s\n", status, servername_dl, (unsigned)serverport_dl, filename_dl);

                if(strcmp(servername, servername_dl) != 0 || serverport != serverport_dl)
                    conn_close(&sock_fd);

                goto redirect;
            }

            /*
            Message in DrWebUpW:
            Your license key file has not been found in the database! Please contact technical support: http://support.drweb.com.
            */
            if(status == 451)
                fprintf(ERRFP, "Error: License key file has not been found in the database.\n");

            /*
            Message in DrWebUpW:
            License key file is blocked!
            */
            if(status == 452)
                fprintf(ERRFP, "Error: License key file is blocked or incorrect UserID/MD5.\n");

            /*
            Message in DrWebUpW:
            You are using an unregistered version of Dr.Web. To receive updates, please register.
            */
            if(status == 600)
                fprintf(ERRFP, "Error: License key file is key from an unregistered version.\n");
        }

        while((tmp = strchr(bufpos, '\r')) != NULL && tmp - 2 != strstr(bufpos - 2, "\r\n\r\n"))
        {
            sscanf(bufpos, "%[^:]", field_name);
            bufpos += strlen(field_name) + 2;
            sscanf(bufpos, "%[^\r]", field_content);
            bufpos += strlen(field_content) + 2;

            if(strcmp(field_name, "Connection") == 0)
            {
                to_lowercase(field_content);
                if(strcmp(field_content, "keep-alive") == 0 && strcmp(servername, servername_dl) == 0)
                    sock_fd_ka = sock_fd; /* Server supports keep-alive */
                else
                    sock_fd_ka = SOCKET_BAD_VALUE;
            }
            else if(strcmp(field_name, "Content-Length") == 0)
            {
                sscanf(field_content, "%lu", & msgsize);
            }
            else if(strcmp(field_name, "Last-Modified") == 0)
            {
                /* https://tools.ietf.org/html/rfc2616#section-3.3.1 */

                struct tm raw_time;
                char month[4];
                const int valid_sscanf_count = 6;
                int current_sscanf_count = 0;

                memset(&raw_time, 0, sizeof(struct tm));
                raw_time.tm_wday = -1;
                raw_time.tm_yday = -1;
                raw_time.tm_isdst = -1;

                /* Sun, 06 Nov 1994 08:49:37 GMT  ; RFC 822, updated by RFC 1123 */
                if(current_sscanf_count != valid_sscanf_count)
                    current_sscanf_count = sscanf(field_content, "%*[^,], %d %[^ ] %d %d:%d:%d",
                                                  & raw_time.tm_mday, month, & raw_time.tm_year,
                                                  & raw_time.tm_hour, & raw_time.tm_min, & raw_time.tm_sec);

                /* Sunday, 06-Nov-94 08:49:37 GMT ; RFC 850, obsoleted by RFC 1036 */
                if(current_sscanf_count != valid_sscanf_count)
                    current_sscanf_count = sscanf(field_content, "%*[^,], %d-%[^-]-%d %d:%d:%d",
                                                  & raw_time.tm_mday, month, & raw_time.tm_year,
                                                  & raw_time.tm_hour, & raw_time.tm_min, & raw_time.tm_sec);

                /* Sun Nov  6 08:49:37 1994       ; ANSI C's asctime() format */
                if(current_sscanf_count != valid_sscanf_count)
                    current_sscanf_count = sscanf(field_content, "%*[^ ] %[^ ] %d %d:%d:%d %d",
                                                  month, & raw_time.tm_mday, & raw_time.tm_hour,
                                                  & raw_time.tm_min, & raw_time.tm_sec, & raw_time.tm_year);

                if(current_sscanf_count == valid_sscanf_count)
                {
                    month[3] = '\0';
                    to_lowercase(month);
                    if     (strcmp(month, "jan") == 0) raw_time.tm_mon = 0;
                    else if(strcmp(month, "feb") == 0) raw_time.tm_mon = 1;
                    else if(strcmp(month, "mar") == 0) raw_time.tm_mon = 2;
                    else if(strcmp(month, "apr") == 0) raw_time.tm_mon = 3;
                    else if(strcmp(month, "may") == 0) raw_time.tm_mon = 4;
                    else if(strcmp(month, "jun") == 0) raw_time.tm_mon = 5;
                    else if(strcmp(month, "jul") == 0) raw_time.tm_mon = 6;
                    else if(strcmp(month, "aug") == 0) raw_time.tm_mon = 7;
                    else if(strcmp(month, "sep") == 0) raw_time.tm_mon = 8;
                    else if(strcmp(month, "oct") == 0) raw_time.tm_mon = 9;
                    else if(strcmp(month, "nov") == 0) raw_time.tm_mon = 10;
                    else if(strcmp(month, "dec") == 0) raw_time.tm_mon = 11;
                    else assert(0);
                    if(raw_time.tm_year >= 1900) raw_time.tm_year -= 1900;

                    lastmod = mktime(& raw_time);
                    if(lastmod > 0)
                        lastmod += tzshift;
                }
                else
                {
                    lastmod = 0;
                    fprintf(ERRFP, "Warning: Can't parse Last-Modified: %s\n", field_content);
                }
            }
            else if(strcmp(field_name, "Transfer-Encoding") == 0)
            {
                to_lowercase(field_content);
                if(strcmp(field_content, "chunked") == 0)
                {
                    is_chunked = 1;
                }
                else if(strcmp(field_content, "identity") == 0)
                {
                    is_chunked = 0;
                }
                /* TODO: Transfer-Encoding: compress/deflate/gzip/mixed is not supported */
                else
                {
                    fprintf(ERRFP, "Error: Unsupported HTTP 1.1 header \"%s: %s\".\n", field_name, field_content);
                    fprintf(ERRFP, "Please consider using the --http-version=1.0 option if problem persists.\n");
                    conn_close(&sock_fd);
                    sock_fd_ka = SOCKET_BAD_VALUE;
                    free(buffer);
                    return EXIT_FAILURE;
                }
            }
        }

        if(tmp != NULL && tmp - 2 == strstr(bufpos - 2, "\r\n\r\n"))
        {
            msgbegin = 1;
            if((ssize_t)(bufend - buffer) - (ssize_t)(tmp + 2 - buffer) > 0)
                bufpos = tmp + 2;
            else
            {
                bufpos = buffer;
                bufend = buffer;
            }
        }
        else
        {
            memmove(buffer, bufpos, (size_t)(bufend - bufpos));
            bufend -= (bufpos - buffer);
            bufpos = buffer;
            * bufend = '\0';
        }
    }

    /* Something wrong */
    if(status != 200 && status != 203)
    {
        if(!socket_good(&sock_fd_ka))
            conn_close(&sock_fd);
        free(buffer);
        return status;
    }

    if(more_verbose)
    {
        printf("[");
        fflush(stdout);
    }
    fp = fopen(filename, "wb"); /* Open result file */
    if(!fp)
    {
        if(more_verbose) printf("\n\n");
        fprintf(ERRFP, "Error with fopen() on %s\n", filename);
        if(!socket_good(&sock_fd_ka))
            conn_close(&sock_fd);
        free(buffer);
        return EXIT_FAILURE;
    }
    if(more_verbose)
    {
        printf("O");
        fflush(stdout);
    }

    if(is_chunked)
        msgsize = 0;
    msgcurr = 0;

    while(1)
    {
        unsigned long chunk_size = 0, chunk_curr = 0;
        if(is_chunked)
        {
            while(bufpos < bufend && (* bufpos == '\r' || * bufpos == '\n'))
                bufpos++;

            if(bufpos != bufend)
            {
                char * tmp = strchr(bufpos, '\r');
                if(tmp)
                {
                    sscanf(bufpos, "%lx", & chunk_size);
                    bufpos = tmp + 2;
                    if(chunk_size == 0)
                        break;
                }
            }
        }
        else
        {
            if(msgcurr != 0)
                break;
            chunk_size = msgsize;
        }

        if(bufpos != bufend) /* Write content */
        {
            chunk_curr = (unsigned long)(bufend - bufpos);
            if((is_chunked == 1 || chunk_size > 0) && chunk_curr > chunk_size)
                chunk_curr = chunk_size;
            if(chunk_curr > 0)
            {
                msgcurr += chunk_curr;
                if(fwrite(bufpos, sizeof(char), chunk_curr, fp) != (size_t)chunk_curr)
                {
                    if(more_verbose) printf("\n\n");
                    fprintf(ERRFP, "Warning: Not all bytes was written\n");
                }
                if(more_verbose)
                {
                    printf("W");
                    fflush(stdout);
                }
                bufpos += chunk_curr;
                if(is_chunked && bufpos != bufend)
                    continue;
            }
        }

        while(chunk_size == 0 || chunk_curr < chunk_size)
        {
            ssize_t recv_count;
            size_t write_count;

            if(bufpos != bufend)
            {
                memmove(buffer, bufpos, (size_t)(bufend - bufpos));
                bufend -= (bufpos - buffer);
                bufpos = buffer;
            }
            else
            {
                bufend = bufpos = buffer;
            }

            assert(is_chunked != 0 || chunk_curr == msgcurr);

            recv_count = recv(sock_fd, bufend, (NETBUFSIZE - (size_t)(bufend - bufpos)), 0);
            if(is_chunked == 0 && recv_count == 0 && chunk_size == 0)
            {
                break;
            }
            if(recv_count <= 0)
            {
#if defined(_WIN32)
                char * wsa_error_str = NULL;
                FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                               NULL, WSAGetLastError(), 0, (LPSTR)(& wsa_error_str), 0, NULL);
                fprintf(ERRFP, "Error %d with recv(): %s", WSAGetLastError(), wsa_error_str);
                LocalFree(wsa_error_str);
#else
                fprintf(ERRFP, "Error %d with recv(): %s\n", errno, strerror(errno));
#endif
                conn_close(&sock_fd);
                free(buffer);
                return EXIT_FAILURE;
            }
            if(more_verbose)
            {
                printf("R");
                fflush(stdout);
            }

            bufend += recv_count;
            * bufend = '\0';

            if(is_chunked && chunk_size == 0)
                break;

            write_count = (size_t)(chunk_size - chunk_curr);
            if(write_count >= (size_t)recv_count)
                write_count = (size_t)recv_count;

            if(fwrite(bufpos, sizeof(char), write_count, fp) != write_count)
            {
                if(more_verbose) printf("\n\n");
                fprintf(ERRFP, "Warning: Not all bytes was written\n");
            }
            if(more_verbose)
            {
                printf("W");
                fflush(stdout);
            }
            bufpos += write_count;
            chunk_curr += (unsigned long)write_count;
            msgcurr += (unsigned long)write_count;
        }
    }

    if(!socket_good(&sock_fd_ka))
        conn_close(&sock_fd); /* Close connection */
    fclose(fp);
    free(buffer);
    if(more_verbose)
    {
        printf("]\n\n");
        fflush(stdout);
    }

    if(lastmod && set_mtime(filename, lastmod) != EXIT_SUCCESS) /* Set last modification time */
        return EXIT_FAILURE;
    chmod(filename, MODE_FILE); /* Change access permissions */

    return EXIT_SUCCESS;
}

/* Download file <filename> */
int download(const char * filename)
{
    int counter = 0, status;
    do
    {
        status = conn_get(filename);
        switch(status)
        {
        /* Correctable error */
        case EXIT_FAILURE:
        case 408:
        case 413:
        case 500:
        case 502:
        case 503:
        case 504:
            sleep(REPEAT_SLEEP);
            counter++;
            break;
        /* No error or fatal error */
        default:
            counter += MAX_REPEAT;
            break;
        }
    }
    while(counter < MAX_REPEAT && status != EXIT_SUCCESS && status != 404);
    if(status == EXIT_SUCCESS) /* Download complete */
        return DL_DOWNLOADED;
    if(status == 404) /* Not found */
        return DL_NOT_FOUND;
    if(status != EXIT_FAILURE)
    {
        fprintf(ERRFP, "Error: Server response %d", status);
        switch(status)
        {
        case 100:
            fprintf(ERRFP, " Continue\n");
            break;
        case 101:
            fprintf(ERRFP, " Switching Protocols\n");
            break;
        case 200:
            fprintf(ERRFP, " OK\n");
            break;
        case 201:
            fprintf(ERRFP, " Created\n");
            break;
        case 202:
            fprintf(ERRFP, " Accepted\n");
            break;
        case 203:
            fprintf(ERRFP, " Non-Authoritative Information\n");
            break;
        case 204:
            fprintf(ERRFP, " No Content\n");
            break;
        case 205:
            fprintf(ERRFP, " Reset Content\n");
            break;
        case 206:
            fprintf(ERRFP, " Partial Content\n");
            break;
        case 300:
            fprintf(ERRFP, " Multiple Choices\n");
            break;
        case 301:
            fprintf(ERRFP, " Moved Permanently\n");
            break;
        case 302:
            fprintf(ERRFP, " Found\n");
            break;
        case 303:
            fprintf(ERRFP, " See Other\n");
            break;
        case 304:
            fprintf(ERRFP, " Not Modified\n");
            break;
        case 305:
            fprintf(ERRFP, " Use Proxy\n");
            break;
        case 307:
            fprintf(ERRFP, " Temporary Redirect\n");
            break;
        case 400:
            fprintf(ERRFP, " Bad Request\n");
            break;
        case 401:
            fprintf(ERRFP, " Unauthorized\n");
            break;
        case 402:
            fprintf(ERRFP, " Payment Required\n");
            break;
        case 403:
            fprintf(ERRFP, " Forbidden\n");
            break;
        case 404:
            fprintf(ERRFP, " Not Found\n");
            break;
        case 405:
            fprintf(ERRFP, " Method Not Allowed\n");
            break;
        case 406:
            fprintf(ERRFP, " Not Acceptable\n");
            break;
        case 407:
            fprintf(ERRFP, " Proxy Authentication Required\n");
            break;
        case 408:
            fprintf(ERRFP, " Request Timeout\n");
            break;
        case 409:
            fprintf(ERRFP, " Conflict\n");
            break;
        case 410:
            fprintf(ERRFP, " Gone\n");
            break;
        case 411:
            fprintf(ERRFP, " Length Required\n");
            break;
        case 412:
            fprintf(ERRFP, " Precondition Failed\n");
            break;
        case 413:
            fprintf(ERRFP, " Request Entity Too Large\n");
            break;
        case 414:
            fprintf(ERRFP, " Request-URI Too Long\n");
            break;
        case 415:
            fprintf(ERRFP, " Unsupported Media Type\n");
            break;
        case 416:
            fprintf(ERRFP, " Requested Range Not Satisfiable\n");
            break;
        case 417:
            fprintf(ERRFP, " Expectation Failed\n");
            break;
        case 500:
            fprintf(ERRFP, " Internal Server Error\n");
            break;
        case 501:
            fprintf(ERRFP, " Not Implemented\n");
            break;
        case 502:
            fprintf(ERRFP, " Bad Gateway\n");
            break;
        case 503:
            fprintf(ERRFP, " Service Unavailable\n");
            break;
        case 504:
            fprintf(ERRFP, " Gateway Timeout\n");
            break;
        case 505:
            fprintf(ERRFP, " HTTP Version Not Supported\n");
            break;
        default:
            fprintf(ERRFP, "\n");
            break;
        }
    }
    return DL_FAILED;
}

/* Download file <filename> and compare checksum <checksum_base>
 * with <checksum_real> using <checksum_func> function */
int download_check(const char * filename, const char * checksum_base, char * checksum_real,
                   int (* checksum_func)(const char *, char *), const char * checksum_desc)
{
    int status;

    if(use_fast && tree && exist(filename)) /* Using fast check */
    {
        const char * checksum_tree = avl_hash(tree, filename);
        if(checksum_tree)
        {
            strcpy(checksum_real, checksum_tree);
            if(verbose)
                printf("%s exist, fast checking %s ", filename, checksum_desc);
            if(strcmp(checksum_base, checksum_real) == 0)
            {
                if(verbose)
                    printf("[LIKELY]\n");
                return DL_EXIST;
            }
            else if(verbose)
                printf("[NOT OK]\n");
        }
    }

    status = checksum_func(filename, checksum_real);
    if(status == EXIT_SUCCESS) /* File exist */
    {
        if(verbose)
            printf("%s exist, checking %s ", filename, checksum_desc);
        if(strcmp(checksum_base, checksum_real) != 0) /* Sum mismatched */
        {
            status = EXIT_FAILURE;
            if(verbose)
                printf("[NOT OK]\n");
        }
        else
        {
            if(verbose)
                printf("[OK]\n");
            return DL_EXIST;
        }
    }

    status = download(filename);
    if(status != DL_DOWNLOADED)
        return status;
    if(checksum_func(filename, checksum_real) != EXIT_SUCCESS)
        return DL_FAILED;

    if(verbose)
        printf("%s downloaded, checking %s ", filename, checksum_desc);
    if(strcmp(checksum_base, checksum_real) != 0)
    {
        if(verbose)
            printf("[NOT OK]\n");
        fprintf(ERRFP, "Warning: %s mismatch (real=\"%s\", base=\"%s\")\n", checksum_desc, checksum_real, checksum_base);
        return DL_TRY_AGAIN;
    }
    else if(verbose)
        printf("[OK]\n");
    return DL_DOWNLOADED;
}
