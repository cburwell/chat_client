#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>

#include "networks.h"

int seq_num = 1;

void client_send_msg(int socket_num, char *dest, char *src, char *msg);
void get_handle_response(int socket_num);
void interpret_command(char *send_buf, char *handle, int socket);
void exit_request(int server_socket);
void handle_loop(int server_socket, int num_handles);
void list_handles(int server_socket);
void print_response(void *recv_buf, int src_socket);
int tcp_send_setup(char *handle, char *host_name, char *port);

int main(int argc, char * argv[])
{
    int socket_num;                    //socket descriptor
    void *send_buf, *recv_buf;         //data buffer
    char *handle;
    fd_set readfds, master;

    /* check command line arguments  */
    if (argc != 4)
    {
        printf("usage: %s handle host-name port-number \n", argv[0]);
	    exit(1);
    }

    /* set up the socket for TCP transmission
     * also sends initial packet */
    socket_num = tcp_send_setup(argv[1], argv[2], argv[3]);
    handle = argv[1];

    /* check that handle is ok */
    get_handle_response(socket_num);
    
    /* initialize data buffer for the packet */
    send_buf = (char *) malloc(BUFF_SIZE);
    recv_buf = (char *) malloc(BUFF_SIZE);

    FD_ZERO(&master);
    FD_SET(STDIN_FILENO, &master);
    FD_SET(socket_num, &master);

    while (1) {
        /* reset fd set, clear send buffer */
        readfds = master;
        memset(send_buf, 0, BUFF_SIZE);
        memset(recv_buf, 0, BUFF_SIZE);

        printf("> ");
        fflush(stdout);

        /* Determine where to read from */
        if (select(socket_num+1, &readfds, NULL, NULL, NULL) > 0) {
            /* Take in client input or receive message */
            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                interpret_command(send_buf, handle, socket_num);
            }
            
            if (FD_ISSET(socket_num, &readfds)) {
                print_response(recv_buf, socket_num);
            }
        }
        else {
            perror("select failed\n");
            exit(-1);
        }
    }

    free(recv_buf);
    free(send_buf);

    close(socket_num);
}

/* Used to create and send a messsage packet. If dest is NULL,
 * the function will create a broadcast packet instead */
void client_send_msg(int socket_num, char *dest, char *src, char *msg)
{
    void *packet, *pos;
    uint8_t dest_len;
    if (dest != NULL)
        dest_len = strlen(dest);
    uint8_t  src_len = strlen(src);
    uint8_t flag;
    int sent, send_len;
    uint32_t seq = htonl(seq_num++);

    if (msg == NULL)
        msg = "";

    if (dest == NULL) {
        flag = BCAST_PKT;
        send_len = sizeof(uint32_t)*8 + 2*sizeof(uint8_t)*8 + src_len + 
            strlen(msg) + 1;
    }
    else {
        flag = MSG_PKT;
        send_len = sizeof(uint32_t)*8 + 3*sizeof(uint8_t)*8 + dest_len + 
            src_len + strlen(msg) + 1;
    }

    packet = malloc(send_len);

    /* sequence number */
    pos = packet;
    memcpy(pos, &seq, sizeof(uint32_t));
   
    /* flag */
    pos += sizeof(uint32_t);
    memcpy(pos, &flag, sizeof(uint8_t));
    pos += sizeof(uint8_t);

    if (dest != NULL) {
        /* destination length */
        memcpy(pos, &dest_len, sizeof(uint8_t));
        
        /* destination handle */
        pos += sizeof(uint8_t);
        memcpy(pos, dest, dest_len);
        pos += dest_len;
    }

    /* source length */
    memcpy(pos, &src_len, sizeof(uint8_t));
    
    /* source handle */
    pos += sizeof(uint8_t);
    memcpy(pos, src, src_len);
    
    /* message */
    pos += src_len;
    pos = memcpy(pos, msg, strlen(msg) + 1);

    /* send package */
    if ((sent = send(socket_num, packet, send_len, 0)) < 0) {
        perror("send error\n");
        exit(-1);
    }

    free(packet);
}

/* Get initial response from server for handle creation */
void get_handle_response(int socket_num)
{
    char *buf, *pos;
    int response_len;

    buf = (char *)malloc(BUFF_SIZE);

    if ((response_len = recv(socket_num, buf, BUFF_SIZE, 0)) < 0) {
        perror("recv error\n");
        exit(-1);
    }

    pos = buf + sizeof(uint32_t);
    switch (*(uint8_t *)pos) {
        case GOOD_HANDLE:
            printf("Welcome!\n");
            break;
        case BAD_HANDLE:
            printf("Error: Handle name already in use\n");
            exit(-1);
            break;
        default:
            printf("Unknown flag, terminating\n");
            exit(-1);
            break;
    }
}

void exit_request(int server_socket)
{
    Header *packet;
    int transfer_len;

    packet = malloc(sizeof(Header));
    packet->seq_num = htonl(seq_num++);
    packet->flag = REQ_EXIT;

    /* Send exit request */
    if ((transfer_len = send(server_socket, packet, sizeof(Header), 0)) < 0) {
        perror("send");
        exit(-1);
    }

    /* Wait for exit ack */
    if ((transfer_len = recv(server_socket, packet, sizeof(Header), 0)) < 0) {
        perror("recv");
        exit(-1);
    }

    /* Terminate */
    if (packet->flag == ACK_EXIT) {
        free(packet);
        exit(0);
    }
    else {
        free(packet);
        perror("Exit not ACKed, terminating...\n");
        exit(-1);
    }
}

void handle_loop(int server_socket, int num_handles)
{
    uint32_t i;
    int transfer_len, send_len, recv_len;
    uint8_t h_len = 0;
    void *pos, *send_handle_req, *recv_handle_pkt;
    char *handle; 

    /* packet to send handle requests out on */
    send_len = sizeof(Header) + sizeof(uint32_t);
    send_handle_req = malloc(send_len);
    
    /* packet to receive handle replies on */
    recv_len = sizeof(Header) + sizeof(uint8_t) + HANDLE_LEN;
    recv_handle_pkt = malloc(recv_len);
            
    /* Request and Receive each handle */
    for (i = 0; i < num_handles; i++) {
        /* Clear buffers */
        memset(send_handle_req, 0, send_len);
        memset(recv_handle_pkt, 0, recv_len);
        
        /* Request handles 0 -> X */
        pos = send_handle_req;
        *(uint32_t *)pos = htonl(seq_num++);
        pos += sizeof(uint32_t);
        *(uint8_t *)pos = REQ_HANDLE;
        pos += sizeof(uint8_t);
        *(uint32_t *)pos = htonl(i);

        if ((transfer_len = send(server_socket, send_handle_req, send_len, 0))
                < 0) {
            perror("send");
            exit(-1);
        }

        /* Receive handles 0 -> X */
        if ((transfer_len = recv(server_socket, recv_handle_pkt, recv_len, 0))
               < 0) {
            perror("recv");
            exit(-1);
        }

        /* Receive & ignore DNE responses */
        pos = recv_handle_pkt + sizeof(uint32_t);
        switch(*(uint8_t *)pos) {
            case REP_HANDLE:
                pos += sizeof(uint8_t);
                h_len = *(uint8_t *)pos;
                pos += sizeof(uint8_t);
                handle = malloc(h_len+1);
                handle[h_len] = '\0';
                memcpy(handle, pos, h_len);
                printf("%s\n", handle);
                break;
            case REP_BAD_HANDLE:
                /* ignore */
                break;
            default:
                /* also ignore unknown flags */
                break;
        }
    }

    free(send_handle_req);
    free(recv_handle_pkt);
}

void list_handles(int server_socket)
{
    void *packet, *pos;
    int transfer_len;
    uint32_t num_handles = 0;

    /* Request number of handles on server */
    packet = (Header *)malloc(sizeof(Header));
    ((Header *)packet)->seq_num = htonl(seq_num++);
    ((Header *)packet)->flag = REQ_NUM_HANDLES;
    
    if ((transfer_len = send(server_socket, packet, sizeof(Header), 0)) < 0) {
        perror("send");
        exit(-1);
    }
    memset(packet, 0, sizeof(Header));
    free(packet);

    /* Receive number of handles from server */
    packet = malloc(sizeof(Header) + sizeof(uint32_t));
    if ((transfer_len = recv(server_socket, packet, sizeof(Header) + 
                    sizeof(uint32_t), 0)) < 0) {
        perror("recv");
        exit(-1);
    }

    pos = packet;
    pos += sizeof(uint32_t);
    if (*(uint8_t *)pos == REP_NUM_HANDLES) {
        pos += sizeof(uint8_t);
        num_handles = *(uint32_t *)pos;
        free(packet);
    }
    else {
        printf("Error receiving number of handles\n");
        free(packet);
        return;
    }

    handle_loop(server_socket, num_handles);
}

void interpret_command(char *send_buf, char *handle, int socket)
{
    int send_len = 0;
    char *tok;

    while ((send_buf[send_len] = getchar()) != '\n' && send_len < 80)
           send_len++;
    send_buf[send_len] = '\0';
            
    /* process the type of packet that you expect */
    tok = strtok(send_buf, " ");
    /* call function depending on the operation */
    if (tok != NULL && tok[0] == '%') {
        switch (toupper(tok[1])) { 
            case 'M':
                tok = strtok(NULL, " ");
                client_send_msg(socket, tok, handle, strtok(NULL, ""));
                break;
            case 'B':
                printf("Broadcasting message...\n");
                client_send_msg(socket, NULL, handle, strtok(NULL, ""));
                break;
            case 'L':
                printf("Connected users:\n");
                list_handles(socket);
                break;
            case 'E':
                printf("Exiting...\n");
                exit_request(socket);
                break;
            default:
                printf("Usage: %%<M,B,L,E> [message]\n");
                break;
        }
    }
    else {
        printf("Usage: %%<M,B,L,E> [message]\n");
    }
}

void print_response(void *recv_buf, int src_socket)
{
    int len, recv_len = 0;
    char *src_handle;

    if ((recv_len = recv(src_socket, recv_buf, BUFF_SIZE, 0)) < 0) {
        perror("recv call\n");
        exit(-1);
    }
    else if (recv_len == 0) {
        perror("Server closed connection\n");
        exit(-1);
    }

    /* check flags and move accordingly */
    recv_buf += sizeof(uint32_t);
    switch (*(uint8_t *)recv_buf) {
        case MSG_PKT:
            recv_buf += sizeof(uint8_t);
            len = *(uint8_t *)recv_buf;
            recv_buf += sizeof(uint8_t) + len;
            break;
        case BCAST_PKT:
            recv_buf += sizeof(uint8_t);
            break;
    }

    /* Move through packet to get to sender data */
    len = *(uint8_t *)recv_buf;
    src_handle = malloc(len+1);
    src_handle[len] = '\0';
    recv_buf += sizeof(uint8_t);
    memcpy(src_handle, recv_buf, len);
    recv_buf += len;

    printf("%s: %s\n", src_handle, (char *)recv_buf);
    free(src_handle);
}
    
int tcp_send_setup(char *handle, char *host_name, char *port)
{
    int socket_num;
    struct sockaddr_in remote;       // socket address for remote side
    struct hostent *hp;              // address of remote host
    void *packet, *ptr;
    uint32_t seq;
    uint8_t flag = INIT_PKT;
    int sent, send_len = 0, len;

    /* create the socket */
    if ((socket_num = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
	    perror("socket call");
	    exit(-1);
	}

    /* designate the addressing family */
    remote.sin_family = AF_INET;

    /* get the address of the remote host and store */
    if ((hp = gethostbyname(host_name)) == NULL)
	{
	  printf("Error getting hostname: %s\n", host_name);
	  exit(-1);
	}
    
    memcpy((char*)&remote.sin_addr, (char*)hp->h_addr, hp->h_length);

    /* get the port used on the remote side and store */
    remote.sin_port = htons(atoi(port));

    if(connect(socket_num, (struct sockaddr*)&remote, 
                sizeof(struct sockaddr_in)) < 0)
    {
	perror("connect call");
	exit(-1);
    }

    /* connected to the server, check if handle is valid */
    len = strlen(handle);
    packet = malloc(sizeof(uint32_t) + sizeof(uint8_t) + len);
    ptr = packet;

    /* sequence number */
    seq = htonl(seq_num++);
    memcpy(packet, &seq, sizeof(uint32_t));
    
    /* flag */
    packet += sizeof(uint32_t);
    memcpy(packet, &flag, sizeof(uint8_t));
   
    /* handle length */
    packet += sizeof(uint8_t);
    memcpy(packet, &len, len);

    /* handle */
    packet += sizeof(uint8_t);
    memcpy(packet, handle, strlen(handle));
    packet = ptr;

    send_len = sizeof(Header)*8 + strlen(handle);
    if ((sent = send(socket_num, packet, send_len, 0)) < 0) {
        perror("send error\n");
        exit(-1);
    }

    return socket_num;
}
