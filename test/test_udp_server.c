// Server side implementation of UDP client-server model
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
  
#define PORT    5000
#define MAXLINE 1024
  
// Driver code
int main() 
{
    int sockfd;
    char buffer[MAXLINE];
    char *hello = "Hello from server";
    struct sockaddr_in servaddr, cliaddr;
    struct ip_mreq mreq;
    uint8_t enable = 1;
      
    // Creating socket file descriptor
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
      
    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char*) &enable, sizeof(enable));
      
    // Filling server information
    servaddr.sin_family    = AF_INET; // IPv4
    servaddr.sin_addr.s_addr = inet_addr("239.255.255.250");
    servaddr.sin_port = htons(1900);
      
    // Bind the socket with the server address
    if ( bind(sockfd, (const struct sockaddr *)&servaddr, 
            sizeof(servaddr)) < 0 )
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &mreq, sizeof(mreq));
    
    while(1)
    {
        int len, n;
    
        len = sizeof(cliaddr);  //len is value/resuslt
    
        n = recvfrom(sockfd, (char *)buffer, MAXLINE, 
                    MSG_WAITALL, ( struct sockaddr *) &cliaddr,
                    &len);
        buffer[n] = '\0';
        printf("Client : %s\n", buffer);
        // sendto(sockfd, (const char *)hello, strlen(hello), 
        //     MSG_CONFIRM, (const struct sockaddr *) &cliaddr,
        //         len);
        // printf("Hello message sent.\n");
    }
      
    return 0;
}