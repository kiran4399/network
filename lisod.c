/*** Author: Kiran Kumar Lekkala (modified)
 ***/


#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <arpa/inet.h>

#include "mio.h"
#include "loglib.h"
#include "cgi.h"

#define BUF_SIZE      8192   /* Initial buff size */
#define MAX_SIZE_HEADER 8192    /* Max length of size info for the incomming msg */
#define ARG_NUMBER    8    /* The number of argument lisod takes*/
#define LISTENQ       1024   /* second argument to listen() */
#define VERBOSE       0    /* Whether to print out debug infomations */
#define DATE_SIZE     35
#define FILETYPE_SIZE 15

/* Functions prototypes */
void usage();
int open_listen_socket(int port);
void init_pool(int listen_sock, Pool *p);
void add_client(int conn_sock, Pool *p, struct sockaddr_in *cli_addr);
void serve_clients(Pool *p);
void server_send(Pool *p);
void clean_state(Pool *p, int listen_sock);

void free_buf(Buff *bufi);
void clienterror(FILE *fd, Requests *req, char *addr, char *cause, char *errnum, char *shortmsg, char *longmsg);
int read_requesthdrs(Buff *b, Requests *req);
void get_time(char *date);
Requests *get_freereq(Buff *b);
void put_header(Requests * req, char *key, char *value);
char * get_header(Requests * req, char *key);
void close_conn(Pool *p, int i);
char * get_header(Requests * req, char *key);
int parse_uri(Pool *p, char *uri, char *filename, char *cgiargs);
void get_filetype(char *filename, char *filetype);
void serve_static(FILE *fd, Buff *b, char *filename, struct stat sbuf);
void put_req(Requests *req, char *method, char *uri, char *version);
int is_valid_method(char *method);
char *get_hdr_value_by_key(Headers *hdr, char *key);

/** @brief Wrapper function for closing socket
 *  @param sock The socket fd to be closed
 *  @return 0 on sucess, 1 on error
 */
int close_socket(int sock) {
    printf("Close sock %d\n", sock);

    if (close(sock)) {
        fprintf(stder
r, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    int listen_sock, client_sock;
    socklen_t cli_size;
    struct sockaddr cli_addr;

    int http_port;  /* The port for the HTTP server to listen on */
    int https_port; /* The port for the HTTPS server to listen on */
    char *log_file; /* File to send log messages to (debug, info, error) */
    char *lock_file; /* File to lock on when becoming a daemon process */
    //char *www;  /* Folder containing a tree to serve as the root of website */
    char *cgi; /* Script where to redirect all CGI URIs */
    char *pri_key; /* Private key file path */
    char *cert;  /* Certificate file path */

    Pool pool;

    if (argc != ARG_NUMBER + 1) {
      usage();
    }

    /* Parse arguments */
    http_port = atoi(argv[1]);
    https_port = atoi(argv[2]);
    log_file = argv[3];
    lock_file = argv[4];
    pool.www = argv[5];
    cgi = argv[6];
    pri_key = argv[7];
    cert = argv[8];

    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout, "----- Echo Server -----\n");

    listen_sock = open_listen_socket(http_port);
    init_pool(listen_sock, &pool);
    pool.logfd = log_init(log_file);
    memset(&cli_addr, 0, sizeof(struct sockaddr));
    memset(&cli_size, 0, sizeof(socklen_t));

    /* finally, loop waiting for input and then write it back */
    while (1) {

        pool.ready_read = pool.read_set;
        pool.ready_write = pool.write_set;
        if (VERBOSE)
            printf("New select\n");
        pool.nready = select(pool.maxfd + 1,
                            &pool.ready_read,
                            &pool.ready_write, NULL, NULL);
        if (VERBOSE)
            printf("nready = %d\n", pool.nready);

        if (pool.nready == -1) {
            /* Something wrong with select */
            if (VERBOSE)
                printf("Select error on %s\n", strerror(errno));
            clean_state(&pool, listen_sock);
        }


        if (FD_ISSET(listen_sock, &pool.ready_read) &&
                    pool.cur_conn <= FD_SETSIZE) {

            if ((client_sock = accept(listen_sock,
                                    (struct sockaddr *) &cli_addr,
                                    &cli_size)) == -1) {
                close(listen_sock);
                fprintf(stderr, "Error accepting connection.\n");
                continue;
            }
            if (VERBOSE)
                printf("New client %d accepted\n", client_sock);
            fcntl(client_sock, F_SETFL, O_NONBLOCK);
            add_client(client_sock, &pool, (struct sockaddr_in *) &cli_addr);
        }
        serve_clients(&pool);
        if (pool.nready)
            server_send(&pool);

    }
    close_socket(listen_sock);
    return EXIT_SUCCESS;
}


/** @brief Print a help message
 *  print a help message and exit.
 *  @return Void
 */
void
usage(void) {
    fprintf(stderr, "usage: ./lisod <HTTP port> <HTTPS port> <log file> "
      "<lock file> <www folder> <CGI script path> <private key file> "
      "<certificate file>\n");
    exit(EXIT_FAILURE);
}

/** @brief Create a socket to lesten
 *  @param port The number of port to be binded
 *  @return The fd of created listenning socket
 */
int open_listen_socket(int port) {
    int listen_socket;
    int yes = 1;        // for setsockopt() SO_REUSEADDR
    struct sockaddr_in addr;

    /* all networked programs must create a socket */
    if ((listen_socket = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Failed creating socket.\n");
        return EXIT_FAILURE;
    }

    // lose the pesky "address already in use" error message
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* servers bind sockets to ports--notify the OS they accept connections */
    if (bind(listen_socket, (struct sockaddr *) &addr, sizeof(addr))) {
        close_socket(listen_socket);
        fprintf(stderr, "Failed binding socket.\n");
        return EXIT_FAILURE;
    }


    if (listen(listen_socket, LISTENQ)) {
        close_socket(listen_socket);
        fprintf(stderr, "Error listening on socket.\n");
        return EXIT_FAILURE;
    }

    return listen_socket;
}

/** @brief Initial the pool of client fds to be select
 *  @param listen_sock The socket on which the server is listenning
 *         while initial, this should be the greatest fd
 *  @param p the pointer to the pool
 *  @return Void
 */
void init_pool(int listen_sock, Pool *p) {
    int i;
    p->maxi = -1;
    for (i = 0; i < FD_SETSIZE; i++)
        p->buf[i] = NULL;

    p->maxfd = listen_sock;
    p->cur_conn = 0;
    FD_ZERO(&p->read_set);
    FD_ZERO(&p->write_set);
    FD_SET(listen_sock, &p->read_set);
}

/** @brief Add a new client fd
 *  @param conn_sock The socket of client to be added
 *  @param p the pointer to the pool
 *  @return Void
 */
void add_client(int conn_sock, Pool *p, struct sockaddr_in *cli_addr) {
    int i;
    Buff *bufi;
    p->cur_conn++;
    p->nready--;
    for (i = 0; i < FD_SETSIZE; i++)
        if (p->buf[i] == NULL) {

            p->buf[i] = (Buff *)malloc(sizeof(Buff));
            bufi = p->buf[i];
            bufi->buf = (char *)malloc(BUF_SIZE);
            bufi->stage = STAGE_MUV;
            bufi->request = (Requests *)malloc(sizeof(Requests));
            bufi->request->response = (char *)malloc(BUF_SIZE);
            bufi->request->next = NULL;
            bufi->request->header = NULL;
            bufi->request->method = NULL;
            bufi->request->uri = NULL;
            bufi->request->version = NULL;
            bufi->request->valid = REQ_INVALID;
            bufi->cur_size = 0;
            bufi->cur_parsed = 0;
            bufi->size = BUF_SIZE;
            bufi->fd = conn_sock;
            inet_ntop(AF_INET, &(cli_addr->sin_addr),
                      bufi->addr, INET_ADDRSTRLEN);
            FD_SET(conn_sock, &p->read_set);

            if (conn_sock > p->maxfd)
                p->maxfd = conn_sock;
            if (i > p->maxi)
                p->maxi = i;
            break;
        }

    if (i == FD_SETSIZE) {
        fprintf(stderr, "Too many client.\n");
        exit(EXIT_FAILURE);
    }
}

/** @brief Perform recv on available sockets in pool
 *  @param p the pointer to the pool
 *  @return Void
 */
void serve_clients(Pool *p) {
    int conn_sock, i;
    ssize_t readret;
    size_t buf_size;
    char method[BUF_SIZE], uri[BUF_SIZE], version[BUF_SIZE];
    char filename[BUF_SIZE], cgiargs[BUF_SIZE];
    char *value;
    int j;
    struct stat sbuf;
    Requests *req;
    Buff *bufi;

    if (VERBOSE)
        printf("entering recv, nready = %d\n", p->nready);

    for (i = 0; (i <= p->maxi) && (p->nready > 0); i++) {
        if (p->buf[i] == NULL)
            continue;
        bufi = p->buf[i];
        conn_sock = bufi->fd;

        if (FD_ISSET(conn_sock, &p->ready_read)) {
            p->nready--;

            if (bufi->stage == STAGE_ERROR)
                continue;
            if (bufi->stage == STAGE_CLOSE)
                continue;

            if (bufi->stage == STAGE_MUV) {
                buf_size = bufi->size - bufi->cur_size;
                readret = mio_recvlineb(conn_sock,
                                        bufi->buf + bufi->cur_size,
                                        buf_size);
                if (readret <= 0) {
                    printf("serve_clients: readret = %d\n", readret);
                    close_conn(p, i);
                    continue;
                }
                bufi->cur_size += readret;
                j = sscanf(bufi->buf, "%s %s %s", method, uri, version);
                if (j < 3) {
                    continue;
                }



                bufi->cur_request = get_freereq(bufi);

                req = bufi->cur_request;
                put_req(req, method, uri, version);
                if (!is_valid_method(method)) {
                    clienterror(p->logfd, req,
                                bufi->addr, method,
                                "505", "Not Implemented",
                                "Liso does not implement this method");
                    bufi->stage = STAGE_ERROR;
                    FD_SET(conn_sock, &p->write_set);
                    continue;
                }

                if (strcasecmp(version, "HTTP/1.1")) {
                    clienterror(p->logfd, req,
                                bufi->addr, version,
                                "501", "HTTP VERSION NOT SUPPORTED",
                                "Liso does not support this http version");
                    bufi->stage = STAGE_ERROR;
                    FD_SET(conn_sock, &p->write_set);
                    continue;
                }
                bufi->stage = STAGE_HEADER;
                bufi->cur_parsed = bufi->cur_size;
            }
            if (bufi->stage == STAGE_HEADER) {
                req = bufi->cur_request;
                j = read_requesthdrs(bufi, req);

                if (j == -2) {
                    clienterror(p->logfd, bufi->cur_request,
                                bufi->addr, "",
                                "400", "Bad Request",
                                "Liso couldn't parse the request");
                    //close_conn(p, i);
                    bufi->stage = STAGE_ERROR;
                    FD_SET(conn_sock, &p->write_set);
                    continue;
                } else if (j == -1) {
                    continue;
                } else
                    bufi->stage = STAGE_BODY;

                if (!strcmp(req->method, "POST")) {
                    if (NULL == get_hdr_value_by_key(req->header, "Content-Length")) {
                        clienterror(p->logfd, bufi->cur_request,
                                bufi->addr, "",
                                "411", "Length Required",
                                "Liso needs Content-Length header");
                        bufi->stage = STAGE_ERROR;
                        FD_SET(conn_sock, &p->write_set);
                        continue;
                    }
                }

                value = get_hdr_value_by_key(req->header, "Connection");
                if (value) {
                    if (VERBOSE)
                        printf("Req->Connection: %s\n", value);
                    if (!strcmp(value, "close"))
                        bufi->stage = STAGE_CLOSE;
                }

            }



            j = parse_uri(p, uri, filename, cgiargs);
            if (stat(filename, &sbuf) < 0) {                     //line:netp:doit:beginnotfound
                clienterror(p->logfd, bufi->cur_request,
                            bufi->addr, filename,
                            "404", "Not found",
                            "Liso couldn't find this file");
                //close_conn(p, i);
                bufi->stage = STAGE_ERROR;
                FD_SET(conn_sock, &p->write_set);
                continue;
            }

            serve_static(p->logfd, bufi, filename, sbuf);

            /* Now we can select this fd to test if it can be sent to */
            FD_SET(conn_sock, &p->write_set);
            if (VERBOSE)
                printf("Server received on %d, request:\n%s",
                    conn_sock, bufi->buf);
            if (bufi->stage != STAGE_ERROR && bufi->stage != STAGE_CLOSE)
                bufi->stage = STAGE_MUV;
            bufi->cur_size = 0;
            bufi->cur_parsed = 0;
            FD_CLR(conn_sock, &p->ready_read); /* Remove it from ready read */


            // while (1) { /* Keep recv, until -1 or short recv encountered */
            //     buf_size = bufi->size - bufi->cur_size;

            //     if ((readret = recv(conn_sock,
            //                     (bufi->buf + bufi->cur_size),
            //                     buf_size, 0)) == -1)
            //         break;

            //     /* At this time readret is guaranteed to be positive */
            //     bufi->cur_size += readret;
            //     if (readret < buf_size)
            //         break; /* short recv */

            //     if (bufi->cur_size >= MAX_SIZE_HEADER) {
            //         fprintf(stderr, "serve_clients: Incomming msg is "
            //             "greater than 8192 bytes\n");
            //         if (close_socket(conn_sock)) {
            //             fprintf(stderr, "Error closing client socket.\n");
            //         }
            //         FD_CLR(conn_sock, &p->read_set);
            //         p->cur_conn--;
            //         free_buf(bufi);
            //         bufi = NULL;
            //         break;
            //     }

            //     /* Double the buff size once half buff size is used */
            //     if (bufi->cur_size >= (bufi->size / 2)) {
            //         bufi->buf = realloc(bufi->buf,
            //                                  bufi->size * 2);
            //         bufi->size *= 2;
            //     }
            // }
            // if (bufi == NULL) /* max buff size reached */
            //     continue;

            // if (readret == -1 && errno == EWOULDBLOCK) { /* Finish recv, would have block */
            //     if (VERBOSE)
            //         printf("serve_clients: read all data, block prevented.\n");
            // } else if (readret <= 0) {
            //     printf("serve_clients: readret = %d\n", readret);
            //     if (close_socket(conn_sock)) {
            //         fprintf(stderr, "Error closing client socket.\n");
            //     }
            //     p->cur_conn--;
            //     free_buf(bufi);
            //     FD_CLR(conn_sock, &p->read_set);
            //     bufi = NULL;
            //     continue;
            // }
            // /* Now we can select this fd to test if it can be sent to */
            // FD_SET(conn_sock, &p->write_set);
            // if (VERBOSE)
            //     printf("Server received %d bytes data on %d\n",
            //         (int)bufi->cur_size, conn_sock);
            // FD_CLR(conn_sock, &p->ready_read); /* Remove it from ready read */
        }
    }
}


/** @brief Perform send on available buffs in pool
 *  @param p the pointer to the pool
 *  @return Void
 */
void server_send(Pool *p) {
    int i, conn_sock;
    ssize_t sendret;
    Requests *req;
    Buff *bufi;
    if (VERBOSE)
        printf("entering send, nready = %d\n", p->nready);

    for (i = 0; (i <= p->maxi) && (p->nready > 0); i++) {
        /* Go thru all pool unit to see if it is a vaild socket
           and is available to write */
        if (p->buf[i] == NULL)
            continue;
        bufi = p->buf[i];
        conn_sock = bufi->fd;

        if (FD_ISSET(conn_sock, &p->ready_write)) {
            req = bufi->request;
            while (req) {
                if (req->valid != REQ_VALID) {
                    req = req->next;
                    continue;
                }

                if ((sendret = mio_sendn(fd, conn_sock, req->response, strlen(req->response))) > 0) {
                    if (VERBOSE)
                        printf("Server send header to %d\n", conn_sock);

                } else {
                    close_conn(p, i);
                    req = req->next;
                    continue;

                }

                if (req->body != NULL) {
                    if ((sendret = mio_sendn(fd, conn_sock, req->body, req->body_size)) > 0) {
                        if (VERBOSE)
                            printf("Server send %d bytes to %d\n",sendret, conn_sock);
                        munmap(req->body, req->body_size);
                        req->body = NULL;
                    } else {

                        close_conn(p, i);
                        req = req->next;
                        continue;
                    }
                }

                req->valid = REQ_INVALID;
                req = req->next;
            }
            if (bufi->stage == STAGE_ERROR || bufi->stage == STAGE_CLOSE) {
                close_conn(p, i);
            }


            // if (bufi->cur_size == 0)  /* Skip if this buf is empty */
            //     continue;

            // if (sendret = mio_sendn(bufi) > 0) {
            //     if (VERBOSE)
            //         printf("Server send %d bytes to %d, (%d in buf)\n",
            //             (int)sendret, conn_sock, bufi->cur_size);
            //     bufi->cur_size = 0;
            // } else {
            //     if (close_socket(conn_sock)) {
            //         fprintf(stderr, "Error closing client socket.\n");
            //     }
            //     p->cur_conn--;
            //     free_buf(bufi);
            //     FD_CLR(conn_sock, &p->read_set);
            //     bufi = NULL;
            // }

            /* Remove it from write set, since server has sent all data
               for this particular socket */
            FD_CLR(conn_sock, &p->write_set);
        }
    }
}


/** @brief Clean up all current connected socket
 *  @param p the pointer to the pool
 *  @return Void
 */
void clean_state(Pool *p, int listen_sock) {
    int i, conn_sock;
    for (i = 0; i <= p->maxi; i++) {
        if (p->buf[i]) {
            conn_sock = p->buf[i]->fd;
            if (close_socket(conn_sock)) {
                fprintf(stderr, "Error closing client socket.\n");
            }
            p->cur_conn--;
            FD_CLR(conn_sock, &p->read_set);
            free_buf(p->buf[i]);
            p->buf[i] = NULL;
        }
    }
    p->maxi = -1;
    p->maxfd = listen_sock;
    p->cur_conn = 0;
}


void free_buf(Buff *bufi) {
    Headers *hdr = NULL;
    Headers *hdr_pre = NULL;
    Requests *req = NULL;
    Requests *req_pre = NULL;
    free(bufi->buf);

    req = bufi->request;

    while (req) {
        req_pre = req;
        req = req->next;

        hdr = req_pre->header;
        while (hdr) {
            hdr_pre = hdr;
            hdr = hdr->next;
            free(hdr_pre);
        }
        if (req_pre->method)
            free(req_pre->method);
        if (req_pre->uri)
            free(req_pre->uri);
        if (req_pre->version)
            free(req_pre->version);
        free(req_pre->response);
        free(req_pre);
    }
    free(bufi);
}


void clienterror(FILE *fd, Requests *req, char *addr, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char date[DATE_SIZE], body[BUF_SIZE], hdr[BUF_SIZE];
    int len = 0;
    get_time(date);
    /* Build the HTTPS response body */
    sprintf(body, "<html><title>Liso</title>");
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Liso Web server</em>\r\n", body);

     /* Print the HTTPS response */
    sprintf(hdr, "HTTP/1.1 %s %s\r\n", errnum, shortmsg);
    sprintf(hdr, "%sContent-Type: text/html\r\n",hdr);
    sprintf(hdr, "%sConnection: close\r\n",hdr);
    sprintf(hdr, "%sDate: %s\r\n",hdr, date);
    sprintf(hdr, "%sContent-Length: %d\r\n\r\n%s",hdr, (int)strlen(body), body);
    len = strlen(hdr);
    req->response = (char *)malloc(len + 1);
    sprintf(req->response, "%s", hdr);
    log_write(fd, req, addr, date, errnum, len);
    req->body = NULL;
    req->valid = REQ_VALID;
}


/** @brief Clean up all current connected socket
 *  @param p the pointer to the pool
 *  @return -1 received a line that does not terminated by \n
 *          -2 received a line that does not match the header format
 */
int read_requesthdrs(Buff *b, Requests *req) {
    int len = 0;
    int i;
    char key[BUF_SIZE];
    char value[BUF_SIZE];
    char *buf = b->buf + b->cur_parsed;


    if (VERBOSE)
        printf("entering read request:\n%s", buf);

    //printf("%d\n", strcmp(buf, "\r\n"));

    while (1) {
        buf += len;
        mio_recvlineb(b->fd, buf, b->size - b->cur_size);
        len = strlen(buf);

        if (len == 0)
            return -1;
        b->cur_size += len;

        if (buf[len - 1] != '\n') return -1;

        if (VERBOSE)
            printf("Receive line:\n%s", buf);

        if (!strcmp(buf, "\r\n"))
            break;

        i = sscanf(b->buf + b->cur_parsed, "%[^':']:%s\r\n", key, value);

        if (i != 2) return -2;

        b->cur_parsed += len;

        put_header(req, key, value);
    }
    b->stage = STAGE_BODY;
    return 1;
}


void get_time(char *date) {
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);
    strftime(date, DATE_SIZE, "%a, %d %b %Y %T %Z", tmp);
}

Requests *get_freereq(Buff *b) {
    Requests *req = b->request;
    while (req->valid == REQ_VALID) {
        if (req->next == NULL)
            break;
        req = req->next;
    }

    if (req->valid == REQ_INVALID) {
        Headers *hdr = req->header;
        Headers *hdr_pre;
        while (hdr) {
            hdr_pre = hdr;
            hdr = hdr->next;
            free(hdr_pre);
        }
        req->header = NULL;
        return req;
    }
    else {
        req->next = (Requests *)malloc(sizeof(Requests));
        return req->next;
    }
}

void put_header(Requests * req, char *key, char *value) {
    Headers *hdr = req->header;
    if (req->header == NULL) {
        req->header = (Headers *)malloc(sizeof(Headers));
        req->header->next = NULL;
        req->header->key = (char *)malloc(strlen(key) + 1);
        req->header->value = (char *)malloc(strlen(value) + 1);
        strcpy(req->header->key, key);
        strcpy(req->header->value, value);
        return;
    }

    while (hdr->next != NULL)
        hdr = hdr->next;

    hdr->next = (Headers *)malloc(sizeof(Headers));
    hdr = hdr->next;
    hdr->next = NULL;
    hdr->key = (char *)malloc(strlen(key) + 1);
    hdr->value = (char *)malloc(strlen(value) + 1);
    strcpy(hdr->key, key);
    strcpy(hdr->value, value);
}


char * get_header(Requests * req, char *key) {
    Headers *hdr = req->header;
    while (hdr != NULL) {
        if (strcasecmp(hdr->key, key)) {
            hdr = hdr->next;
            continue;
        } else {
            return hdr->value;
        }
    }
    return NULL;
}


void close_conn(Pool *p, int i) {
    //if (p->buf[i] == NULL)
        //return;
    int conn_sock = p->buf[i]->fd;
    if (close_socket(conn_sock)) {
        fprintf(stderr, "Error closing client socket.\n");
    }
    p->cur_conn--;
    free_buf(p->buf[i]);
    FD_CLR(conn_sock, &p->read_set);
    FD_CLR(conn_sock, &p->write_set);
    p->buf[i] = NULL;
}

int parse_uri(Pool *p, char *uri, char *filename, char *cgiargs) {
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {
        strcpy(cgiargs, "");
        strcpy(filename, p->www);
        strcat(filename, uri);
        if (uri[strlen(uri)-1] == '/')
            strcat(filename, "index.html");
        return 1;
    } else {  /* Dynamic content */
        ptr = index(uri, '?');
        if (ptr) {
            strcpy(cgiargs, ptr+1);
            *ptr = '\0';
        } else
            strcpy(cgiargs, "");
        strcpy(filename, p->www);
        strcat(filename, uri);
        return 0;
    }
}



void get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".css"))
        strcpy(filetype, "text/css");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else
        strcpy(filetype, "text/plain");
}

void put_req(Requests *req, char *method, char *uri, char *version) {
    req->method = (char *)malloc(strlen(method) + 1);
    req->uri = (char *)malloc(strlen(uri) + 1);
    req->version = (char *)malloc(strlen(version) + 1);
    strcpy(req->method, method);
    strcpy(req->uri, uri);
    strcpy(req->version, version);
}



void serve_static(FILE *fd, Buff *b, char *filename, struct stat sbuf) {
    int srcfd;
    int filesize = sbuf.st_size;
    int len = 0;
    char *srcp, filetype[FILETYPE_SIZE], date[DATE_SIZE], buf[BUF_SIZE];
    Requests *req = b->cur_request;
    char modify_time[DATE_SIZE];


    strftime(modify_time, DATE_SIZE, "%a, %d %b %Y %T %Z", localtime(&sbuf.st_mtime));

    get_time(date);
    /* Send response headers to client */
    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.1 200 OK\r\n");
    sprintf(buf, "%sServer: Liso/1.0\r\n", buf);
    sprintf(buf, "%sDate:%s\r\n", buf, date);
    sprintf(buf, "%sConnection:keep-alive\r\n", buf);
    sprintf(buf, "%sContent-Length: %d\r\n", buf, filesize);
    sprintf(buf, "%sLast-Modified:%s\r\n", buf, modify_time);
    sprintf(buf, "%sContent-Type: %s\r\n\r\n", buf, filetype);

    len = strlen(buf);
    req->response = (char *)malloc(len + 1);
    sprintf(req->response, "%s", buf);

    if (strcmp(req->method, "HEAD")) {
        srcfd = open(filename, O_RDONLY, 0);
        srcp = mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        req->body = srcp;
        req->body_size = filesize;
        close(srcfd);
        log_write(fd, req, b->addr, date, "200", len + filesize);
    } else {
        log_write(fd, req, b->addr, date, "200", len);
    }



    req->valid = REQ_VALID;
}


/** @brief To check if the method is implemented
 *  @param method the pointer to the string containing method
 *  @return 1 on yes 0 on no.
 */
int is_valid_method(char *method) {
    if (!strcasecmp(method, "GET")) {
        return 1;
    } else if (!strcasecmp(method, "POST")) {
        return 1;
    } else if (!strcasecmp(method, "HEAD")) {
        return 1;
    }
    return 0;
}

char *get_hdr_value_by_key(Headers *hdr, char *key) {
    while (hdr) {
        if(!strcmp(hdr->key, key)) {
            return hdr->value;
        }
        hdr = hdr->next;
    }
    return NULL;
}
