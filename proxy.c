#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE (MAX_CACHE_SIZE / MAX_OBJECT_SIZE)  /* 大约 10 */
#define NTHREADS  8
#define SBUFSIZE  16
//
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct {
    int *buf;          /* Buffer array */         
    int n;             /* Maximum number of slots */
    int front;         /* buf[(front+1)%n] is first item */
    int rear;          /* buf[rear%n] is last item */
    sem_t mutex;       /* Protects accesses to buf */
    sem_t slots;       /* Counts available slots */
    sem_t items;       /* Counts available items */
} sbuf_t;

sbuf_t sbuf;

struct Uri {
    char host[MAXLINE]; 
    char port[MAXLINE]; 
    char path[MAXLINE]; 
};

typedef struct {
    char obj[MAX_OBJECT_SIZE];
    char uri[MAXLINE];
    int LRU;
    int isEmpty;

    int read_cnt; 
    sem_t w;      
    sem_t mutex;  
} block;

typedef struct {
    block data[MAX_CACHE];
    int num;
} Cache;

Cache cache;

void doit(int fd);
void read_requesthdrs(rio_t *rp);
void build_header(char *buf, struct Uri *uri_data, rio_t *client_rio);
void parse_uri(char *uri, struct Uri *uri_data);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);
void *thread(void *vargp);
void sbuf_init(sbuf_t *sp, int n);

void init_Cache() {
    cache.num = 0;
    for(int i=0;i<MAX_CACHE;i++){
        cache.data[i].isEmpty = 1;
        cache.data[i].LRU = 0;
        cache.data[i].read_cnt = 0;
        Sem_init(&cache.data[i].mutex, 0, 1);
        Sem_init(&cache.data[i].w, 0, 1);
    }
}

// 根据 URI 查找缓存，返回 index 或 -1
int get_Cache(char *uri) {
    int idx = -1;
    for(int i=0;i<MAX_CACHE;i++){
        if(!cache.data[i].isEmpty && strcmp(cache.data[i].uri, uri)==0){
            idx = i;
            break;
        }
    }
    if(idx != -1) {
        // 更新 LRU
        cache.data[idx].LRU = 0;
        for(int i=0;i<MAX_CACHE;i++)
            if(!cache.data[i].isEmpty && i!=idx)
                cache.data[i].LRU++;
    }
    return idx;
}

// 写入缓存（完整响应）
void write_Cache(char *uri, char *buf, int size){
    int idx = -1;
    // 找空槽
    for(int i=0;i<MAX_CACHE;i++){
        if(cache.data[i].isEmpty){
            idx = i;
            break;
        }
    }
    // 没空槽就找 LRU 最大的替换
    if(idx == -1){
        int max_LRU = -1;
        for(int i=0;i<MAX_CACHE;i++){
            if(cache.data[i].LRU > max_LRU){
                max_LRU = cache.data[i].LRU;
                idx = i;
            }
        }
    }

    P(&cache.data[idx].w);
    strncpy(cache.data[idx].uri, uri, MAXLINE);
    memcpy(cache.data[idx].obj, buf, size);
    cache.data[idx].isEmpty = 0;
    cache.data[idx].LRU = 0;
    V(&cache.data[idx].w);

    // 其他块 LRU +1
    for(int i=0;i<MAX_CACHE;i++){
        if(i!=idx && !cache.data[i].isEmpty)
            cache.data[i].LRU++;
    }
}

int main(int argc, char **argv)
{
    init_Cache();
    int listenfd, connfd;
    socklen_t clientlen;
    char hostname[MAXLINE], port[MAXLINE];
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage :%s <port> \n", argv[0]);
        exit(1);
    }
    listenfd = Open_listenfd(argv[1]);

    sbuf_init(&sbuf, SBUFSIZE);
    for(int i = 0; i < NTHREADS; i++) {
        Pthread_create(&tid, NULL, thread, NULL);
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s).\n", hostname, port);
    }
    return 0;
}

void sbuf_init(sbuf_t *sp, int n)
{
    sp->buf = Calloc(n, sizeof(int));  
    sp->n = n;
    sp->front = sp->rear = 0;
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->slots, 0, n);  
    Sem_init(&sp->items, 0, 0);  
}

void sbuf_insert(sbuf_t *sp, int item)
{
    P(&sp->slots);                          
    P(&sp->mutex);                          
    sp->buf[(++sp->rear)%(sp->n)] = item;   
    V(&sp->mutex);                          
    V(&sp->items);                          
}

int sbuf_remove(sbuf_t *sp)
{
    int item;
    P(&sp->items);                          
    P(&sp->mutex);                          
    item = sp->buf[(++sp->front)%(sp->n)];  
    V(&sp->mutex);                          
    V(&sp->slots);                          
    return item;
}

void *thread(void *vargp)
{
    Pthread_detach(pthread_self());
    while(1){
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);  // 确保无论如何都关闭
    }
}

/* ============================ doit ============================ */
void doit(int connfd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    rio_t rio;
    Rio_readinitb(&rio, connfd);

    if(!Rio_readlineb(&rio, buf, MAXLINE)) return;
    sscanf(buf, "%s %s %s", method, uri, version);

    if(strcasecmp(method, "GET")){
        clienterror(connfd, method, "501", "Not Implemented", "Proxy does not implement this method");
        return;
    }

    // 检查缓存
    int idx;
    if((idx = get_Cache(uri)) != -1){
        P(&cache.data[idx].mutex);
        cache.data[idx].read_cnt++;
        if(cache.data[idx].read_cnt == 1) P(&cache.data[idx].w);
        V(&cache.data[idx].mutex);

        Rio_writen(connfd, cache.data[idx].obj, strlen(cache.data[idx].obj));

        P(&cache.data[idx].mutex);
        cache.data[idx].read_cnt--;
        if(cache.data[idx].read_cnt==0) V(&cache.data[idx].w);
        V(&cache.data[idx].mutex);
        return;
    }

    // 解析 URI
    struct Uri uri_data;
    parse_uri(uri, &uri_data);

    // 构建请求 header
    char request[MAXLINE*10]; // 足够大
    build_header(request, &uri_data, &rio);

    // 连接服务器
    int serverfd = Open_clientfd(uri_data.host, uri_data.port);
    if(serverfd < 0){
        clienterror(connfd, uri_data.host, "502", "Bad Gateway", "Cannot connect to server");
        return;
    }

    // 发送请求
    Rio_writen(serverfd, request, strlen(request));

    // 读取服务器响应
    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd);

    char cache_buf[MAX_OBJECT_SIZE];
    int cache_size = 0;
    ssize_t n;
    char tmpbuf[MAXLINE];

    while((n = Rio_readnb(&server_rio, tmpbuf, MAXLINE)) > 0){
        Rio_writen(connfd, tmpbuf, n);
        if(cache_size + n < MAX_OBJECT_SIZE){
            memcpy(cache_buf + cache_size, tmpbuf, n);
            cache_size += n;
        }
    }
    Close(serverfd);

    // 写入缓存
    if(cache_size < MAX_OBJECT_SIZE){
        write_Cache(uri, cache_buf, cache_size);
    }
}

void parse_uri(char *uri, struct Uri *uri_data) {
    strcpy(uri_data->port, "80");
    strcpy(uri_data->path, "/");
    
    char *host_start = uri;
    char *path_start = NULL;
    char *port_start = NULL;
    
    if (strstr(uri, "//")) {
        host_start = strstr(uri, "//") + 2;
    }
    
    path_start = strchr(host_start, '/');
    port_start = strchr(host_start, ':');
    
    if (port_start && (!path_start || port_start < path_start)) {
        size_t host_len = port_start - host_start;
        strncpy(uri_data->host, host_start, host_len);
        uri_data->host[host_len] = '\0';
        
        if (path_start) {
            size_t port_len = path_start - port_start - 1;
            strncpy(uri_data->port, port_start + 1, port_len);
            uri_data->port[port_len] = '\0';
            strcpy(uri_data->path, path_start);
        } else {
            strcpy(uri_data->port, port_start + 1);
        }
    } else {
        if (path_start) {
            size_t host_len = path_start - host_start;
            strncpy(uri_data->host, host_start, host_len);
            uri_data->host[host_len] = '\0';
            strcpy(uri_data->path, path_start);
        } else {
            strcpy(uri_data->host, host_start);
        }
    }
    
    if (strlen(uri_data->path) == 0) {
        strcpy(uri_data->path, "/");
    }
}

/* ============================ build_header ============================ */
void build_header(char *buf, struct Uri *uri_data, rio_t *client_rio) {
    char request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
    char line[MAXLINE];

    sprintf(request_hdr, "GET %s HTTP/1.0\r\n", uri_data->path);
    sprintf(host_hdr, "Host: %s\r\n", uri_data->host);

    other_hdr[0] = '\0';
    Rio_readlineb(client_rio, line, MAXLINE);
    while (strcmp(line, "\r\n")) {
        if (strncasecmp(line, "Host:", 5) != 0 &&
            strncasecmp(line, "Connection:", 11) != 0 &&
            strncasecmp(line, "Proxy-Connection:", 17) != 0 &&
            strncasecmp(line, "User-Agent:", 11) != 0) {
            strcat(other_hdr, line);
        }
        Rio_readlineb(client_rio, line, MAXLINE);
    }

    sprintf(buf, "%s%s%s%s%s%s\r\n",
            request_hdr,
            host_hdr,
            "Connection: close\r\n",
            "Proxy-Connection: close\r\n",
            user_agent_hdr,
            other_hdr);
}


/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}
/* $end clienterror */
