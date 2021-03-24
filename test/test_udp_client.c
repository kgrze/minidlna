// Client side implementation of UDP client-server model
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

void SendSSDPTest(void)
{
    int sockfd;
    char buffer[1024];
    char *ssdp_byebye = 
    "NOTIFY * HTTP/1.1\r\n"
    "HOST:239.255.255.250:1900\r\n"
    "NT:uuid:4d696e69-444c-164e-9d41-080027237071\r\n"
    "USN:uuid:4d696e69-444c-164e-9d41-080027237071\r\n"
    "NTS:ssdp:byebye\r\n"
    "\r\n";

    struct sockaddr_in servaddr;
  
    // Creating socket file descriptor
    if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
  
    memset(&servaddr, 0, sizeof(servaddr));
      
    // Filling server information
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(1900);
    servaddr.sin_addr.s_addr = inet_addr("239.255.255.250");
      
    int n, len;
      
    sendto(sockfd, (const char *)ssdp_byebye, strlen(ssdp_byebye),
        MSG_CONFIRM, (const struct sockaddr *) &servaddr, 
            sizeof(servaddr));
    printf("Hello message sent.\n");
          
    n = recvfrom(sockfd, (char *)buffer, 1024, 
                MSG_WAITALL, (struct sockaddr *) &servaddr,
                &len);
    buffer[n] = '\0';
    printf("Server : %s\n", buffer);
  
    close(sockfd);
}
  
// Driver code
int main() 
{
    SendSSDPTest();
    return 0;
}