#define _GNU_SOURCE
#include "HttpRequest.h"
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include "TcpConnection.h"
#include <assert.h>
#include <ctype.h>

#define HeaderSize 12
struct HttpRequest* httpRequestInit()
{
    struct HttpRequest* request = (struct HttpRequest*)malloc(sizeof(struct HttpRequest));
    httpRequestReset(request);
    request->reqHeaders = (struct RequestHeader*)malloc(sizeof(struct RequestHeader) * HeaderSize);
    return request;
}

void httpRequestReset(struct HttpRequest* req)
{
    req->curState = ParseReqLine;
    req->method = NULL;
    req->url = NULL;
    req->version = NULL;
    req->reqHeadersNum = 0;
}

void httpRequestResetEx(struct HttpRequest* req)
{
    free(req->url);
    free(req->method);
    free(req->version);
    if (req->reqHeaders != NULL)
    {
        for (int i = 0; i < req->reqHeadersNum; ++i)
        {
            free(req->reqHeaders[i].key);
            free(req->reqHeaders[i].value);
        }
        free(req->reqHeaders);
    }
    httpRequestReset(req);
}

void httpRequestDestroy(struct HttpRequest* req)
{
    if (req != NULL)
    {
        httpRequestResetEx(req);
        free(req);
    }
}

void httpRequestAddHeader(struct HttpRequest* request, const char* key, const char* value)
{
    request->reqHeaders[request->reqHeadersNum].key = (char*)key;
    request->reqHeaders[request->reqHeadersNum].value = (char*)value;
    request->reqHeadersNum++;
}

char* httpRequestGetHeader(struct HttpRequest* request, const char* key)
{
    if (request != NULL)
    {
        for (int i = 0; i < request->reqHeadersNum; ++i)
        {
            if (strncasecmp(request->reqHeaders[i].key, key, strlen(key)) == 0)
            {
                return request->reqHeaders[i].value;
            }
        }
    }
    return NULL;
}

char* splitRequestLine(const char* start, const char* end, const char* sub, char** ptr)
{
    char* space = end;
    if (sub != NULL)
    {
        space = memmem(start, end - start, sub, strlen(sub));
        assert(space != NULL);
    }
    int length = space - start;
    char* tmp = (char*)malloc(length + 1);
    strncpy(tmp, start, length);
    tmp[length] = '\0';
    *ptr = tmp;
    return space + 1;
}

bool parseHttpRequestLine(struct HttpRequest* request, struct Buffer* readBuf)
{
    // ???????????????, ???????????????????????????
    char* end = bufferFindCRLF(readBuf);
    // ???????????????????????????
    char* start = readBuf->data + readBuf->readPos;
    // ??????????????????
    int lineSize = end - start;

    if (lineSize)
    {
        start = splitRequestLine(start, end, " ", &request->method);
        start = splitRequestLine(start, end, " ", &request->url);
        splitRequestLine(start, end, NULL, &request->version);
#if 0
        // get /xxx/xx.txt http/1.1
        // ????????????
        char* space = memmem(start, lineSize, " ", 1);
        assert(space != NULL);
        int methodSize = space - start;
        request->method = (char*)malloc(methodSize + 1);
        strncpy(request->method, start, methodSize);
        request->method[methodSize] = '\0';

        // ?????????????????????
        start = space + 1;
        char* space = memmem(start, end-start, " ", 1);
        assert(space != NULL);
        int urlSize = space - start;
        request->url = (char*)malloc(urlSize + 1);
        strncpy(request->url, start, urlSize);
        request->method[urlSize] = '\0';

        // http ??????
        start = space + 1;
        //char* space = memmem(start, end - start, " ", 1);
        //assert(space != NULL);
        //int urlSize = space - start;
        request->version = (char*)malloc(end - start + 1);
        strncpy(request->url, start, end - start);
        request->method[end - start] = '\0';
#endif

        // ???????????????????????????
        readBuf->readPos += lineSize;
        readBuf->readPos += 2;
        // ????????????
        request->curState = ParseReqHeaders;
        return true;
    }
    return false;
}

// ????????????????????????????????????
bool parseHttpRequestHeader(struct HttpRequest* request, struct Buffer* readBuf)
{
    char* end = bufferFindCRLF(readBuf);
    if (end != NULL)
    {
        char* start = readBuf->data + readBuf->readPos;
        int lineSize = end - start;
        // ??????: ???????????????
        char* middle = memmem(start, lineSize, ": ", 2);
        if (middle != NULL)
        {
            char* key = malloc(middle - start + 1);
            strncpy(key, start, middle - start);
            key[middle - start] = '\0';

            char* value = malloc(end - middle - 2 + 1);
            strncpy(value, middle + 2, end - middle - 2);
            value[end - middle - 2] = '\0';

            httpRequestAddHeader(request, key, value);
            // ????????????????????????
            readBuf->readPos += lineSize;
            readBuf->readPos += 2;
        }
        else
        {
            // ????????????????????????, ????????????
            readBuf->readPos += 2;
            // ??????????????????
            // ?????? post ??????, ?????? get ????????????
            request->curState = ParseReqDone;
        }
        return true;
    }
    return false;
}

bool parseHttpRequest(struct HttpRequest* request, struct Buffer* readBuf,
    struct HttpResponse* response, struct Buffer* sendBuf, int socket)
{
    bool flag = true;
    while (request->curState != ParseReqDone)
    {
        switch (request->curState)
        {
        case ParseReqLine:
            flag = parseHttpRequestLine(request, readBuf);
            break;
        case ParseReqHeaders:
            flag = parseHttpRequestHeader(request, readBuf);
            break;
        case ParseReqBody:
            break;
        default:
            break;
        }
        if (!flag)
        {
            return flag;
        }
        // ???????????????????????????, ???????????????, ???????????????????????????
        if (request->curState == ParseReqDone)
        {
            // 1. ??????????????????????????????, ?????????????????????????????????
            processHttpRequest(request, response);
            // 2. ???????????????????????????????????????
            httpResponsePrepareMsg(response, sendBuf, socket);
        }
    }
    request->curState = ParseReqLine;   // ????????????, ???????????????????????????????????????????????????
    return flag;
}

// ????????????get???http??????
bool processHttpRequest(struct HttpRequest* request, struct HttpResponse* response)
{
    if (strcasecmp(request->method, "get") != 0)
    {
        return -1;
    }
    decodeMsg(request->url, request->url);
    // ????????????????????????????????????(??????????????????)
    char* file = NULL;
    if (strcmp(request->url, "/") == 0)
    {
        file = "./";
    }
    else
    {
        file = request->url + 1;
    }
    // ??????????????????
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1)
    {
        // ??????????????? -- ??????404
        //sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
        //sendFile("404.html", cfd);
        strcpy(response->fileName, "404.html");
        response->statusCode = NotFound;
        strcpy(response->statusMsg, "Not Found");
        // ?????????
        httpResponseAddHeader(response, "Content-type", getFileType(".html"));
        response->sendDataFunc = sendFile;
        return 0;
    }

    strcpy(response->fileName, file);
    response->statusCode = OK;
    strcpy(response->statusMsg, "OK");
    // ??????????????????
    if (S_ISDIR(st.st_mode))
    {
        // ?????????????????????????????????????????????
        //sendHeadMsg(cfd, 200, "OK", getFileType(".html"), -1);
        //sendDir(file, cfd);
        // ?????????
        httpResponseAddHeader(response, "Content-type", getFileType(".html"));
        response->sendDataFunc = sendDir;
    }
    else
    {
        // ????????????????????????????????????
        //sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        //sendFile(file, cfd);
        // ?????????
        char tmp[12] = { 0 };
        sprintf(tmp, "%ld", st.st_size);
        httpResponseAddHeader(response, "Content-type", getFileType(file));
        httpResponseAddHeader(response, "Content-length", tmp);
        response->sendDataFunc = sendFile;
    }

    return false;
}

enum HttpRequestState httpRequestState(struct HttpRequest* request)
{
    return request->curState;
}

// ???????????????????????????
int hexToDec(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

// ??????
// to ???????????????????????????, ????????????, from??????????????????, ????????????
void decodeMsg(char* to, char* from)
{
    for (; *from != '\0'; ++to, ++from)
    {
        // isxdigit -> ?????????????????????16????????????, ????????? 0-f
        // Linux%E5%86%85%E6%A0%B8.jpg
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
        {
            // ???16???????????? -> ????????? ????????????????????????????????? int -> char
            // B2 == 178
            // ???3?????????, ?????????????????????, ??????????????????????????????
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);

            // ?????? from[1] ??? from[2] ??????????????????????????????????????????
            from += 2;
        }
        else
        {
            // ????????????, ??????
            *to = *from;
        }

    }
    *to = '\0';
}

const char* getFileType(const char* name)
{
    // a.jpg a.mp4 a.html
    // ?????????????????????.?????????, ??????????????????NULL
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8";	// ?????????
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}

void sendDir(const char* dirName, struct Buffer* sendBuf, int cfd)
{
    char buf[4096] = { 0 };
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent** namelist;
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; ++i)
    {
        // ??????????????? namelist ?????????????????????????????? struct dirent* tmp[]
        char* name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode))
        {
            // a?????? <a href="">name</a>
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>",
                name, name, st.st_size);
        }
        else
        {
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                name, name, st.st_size);
        }
        // send(cfd, buf, strlen(buf), 0);
        bufferAppendString(sendBuf, buf);
#ifndef MSG_SEND_AUTO
        bufferSendData(sendBuf, cfd);
#endif
        memset(buf, 0, sizeof(buf));
        free(namelist[i]);
    }
    sprintf(buf, "</table></body></html>");
    // send(cfd, buf, strlen(buf), 0);
    bufferAppendString(sendBuf, buf);
#ifndef MSG_SEND_AUTO
    bufferSendData(sendBuf, cfd);
#endif
    free(namelist);
}


void sendFile(const char* fileName, struct Buffer* sendBuf, int cfd)
{
    // 1. ????????????
    int fd = open(fileName, O_RDONLY);
    assert(fd > 0);
#if 1
    while (1)
    {
        char buf[1024];
        int len = read(fd, buf, sizeof buf);
        if (len > 0)
        {
            // send(cfd, buf, len, 0);
            bufferAppendData(sendBuf, buf, len);
#ifndef MSG_SEND_AUTO
            bufferSendData(sendBuf, cfd);
#endif
        }
        else if (len == 0)
        {
            break;
        }
        else
        {
            close(fd);
            perror("read");
        }
    }
#else
    off_t offset = 0;
    int size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    while (offset < size)
    {
        int ret = sendfile(cfd, fd, &offset, size - offset);
        printf("ret value: %d\n", ret);
        if (ret == -1 && errno == EAGAIN)
        {
            printf("?????????...\n");
        }
    }
#endif
    close(fd);
}

