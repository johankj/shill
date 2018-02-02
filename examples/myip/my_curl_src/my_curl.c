/*
 * ** my_curl.c -- a stream socket socks 5 client demo
 * Compiled with: gcc my_curl.c -o my_curl
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#define PORT "1080" // the port client will be connecting to

#define MAXDATASIZE 4096 // max number of bytes we can get at once

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
  int sockfd, numbytes;
  char buf[MAXDATASIZE];
  struct hostent *server;
  struct sockaddr_in serv_addr;
  struct addrinfo *servinfo, *p;
  int rv;
  char s[INET6_ADDRSTRLEN];

  int portno = 80;
  char *host = "ifconfig.co";

  puts("MY_CURL");

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
      printf("Could not create socket");
  }
  puts("Socket created");


  /* lookup the ip address */
  server = gethostbyname(host);
  if (server == NULL) {
      perror("ERROR, no such host");
      return 1;
  }

  /* fill in the structure */
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(portno);
  memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

  /* connect the socket */
  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      perror("ERROR connecting");
      return 1;
  }

  /* getc(stdin); */

  puts("Connected\n");

  char *bufferf[4096];

  char* getRequest = "GET /json HTTP/1.1\n"
      "Host: ifconfig.co\n"
      "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10.12; rv:44.0) Gecko/20100101 Firefox/44.0\n"
      "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\n"
      "Accept-Language: en-US,en;q=0.5\n"
      /* "Accept-Encoding: gzip, deflate\n" */
      "Connection: keep-alive\n\n";

  /* printf("%i\n", strlen(getRequest)); */
  if (send(sockfd, getRequest, strlen(getRequest), 0) == -1)
      perror("send err");

  if ((numbytes = recv(sockfd, bufferf, MAXDATASIZE-1, 0)) == -1) {
      perror("recv");
      exit(1);
  }

  printf("client: received:\n%s\n", bufferf);

  close(sockfd);

  return 0;
}
