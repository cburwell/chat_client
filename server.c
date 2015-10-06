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

#include "networks.h"

/* Structure for a global master list of clients */
typedef struct Node {
    char handle[HANDLE_LEN];
    int socket;
    struct Node *next;
} Node;

Node *head = NULL;      // Head of master list
int seq_num = 1;        // Current sequence number
int max = 0;            // Current maximum fd
int server_socket;      // Socket the server opens on

int tcp_recv_setup();
void init_check(void *packet, int socket);
void add_client(int server_socket, fd_set *master);
void remove_client(int client_socket, fd_set *master);
void handle_message(int client_socket, void *buf, fd_set *readfds, 
        fd_set *master);
int check_handle(char *handle);
void forward_message(void *packet, int send_len);
void send_message(char *dest_handle, void *packet, int send_len);
void broadcast_message(int src_socket, void *packet, int send_len, 
        fd_set *readfds);
void clear_list();

int main(int argc, char *argv[])
{
    void *buf;
    fd_set readfds, master;
    int i;

    server_socket = 0;
    
    /* create packet buffer */
    buf = (char *) malloc(BUFF_SIZE);

    /* create the server socket and start listening */
    server_socket = tcp_recv_setup();
    max = server_socket+1;

    FD_ZERO(&master);
    FD_SET(server_socket, &master);

    while (1) {
        /* reset readfds */
        readfds = master;
        memset(buf, 0, BUFF_SIZE);
        
        /* Look for client to serve */
        if (select(max, &readfds, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(-1);
        }

        /* Something to do, find the client */
        for (i = 0; i < max; i++) {
            if (FD_ISSET(i, &readfds)) {
                /* New client */
                if (i == server_socket) {
                    add_client(server_socket, &master);
                }
                /* Client sent a message */
                else {
                    handle_message(i, buf, &readfds, &master); 
                }
            }
        }
    }

    /* close the sockets */
    clear_list(&master);
    close(server_socket);
}

/* Handles the initial packet and adds the client to the global list */
void init_check(void *packet, int socket)
{
    Node *temp = head;
    uint8_t len;
    Header *header;
    int sent, ready = 0;

    /* Begin setting up packet to send */
    header = malloc(sizeof(Header));
    header->seq_num = seq_num++;
    header->flag = BAD_HANDLE;
    
    packet += sizeof(uint32_t) + sizeof(uint8_t);
    len = *(uint8_t *)packet;
    packet += sizeof(uint8_t);
  
    /* Make sure the handle is not taken */
    temp = head;
    while (temp != NULL) {
        /* Handle already exists, make them exit */
        if (temp->handle[0] != 0 && memcmp(temp->handle, packet, len) == 0) {
            header->flag = BAD_HANDLE;
            ready = 1;
            break;
        }
        temp = temp->next;
    }
    
    /* socket should already be in list, find it and name it */
    if (ready == 0) {
        temp = head;
        while (temp != NULL) {
            if (temp->socket == socket) {
                memcpy(temp->handle, packet, len);
                header->flag = GOOD_HANDLE;
                ready = 1;
                break;
            }
            temp = temp->next;
        }
    }

    /* Transmit packet */
    if ((sent = send(socket, header, sizeof(Header), 0)) < 0) {
        perror("send error\n");
        exit(-1);
    }

    free(header);

    return;
}

/* Acknowledge client exit and remove them from the global list
 * and master fd_set */
void ack_exit(int client_socket, void *packet, fd_set *master)
{
    int transfer_len;

    ((Header *)packet)->flag = ACK_EXIT;

    if ((transfer_len = send(client_socket, packet, sizeof(Header), 0)) < 0) {
        perror("send");
        exit(-1);
    }
    
    remove_client(client_socket, master);
}

/* Send the current number of clients connected */
void send_handle_count(int client_socket)
{
    uint32_t count = 0;
    Node *temp = head;
    void *packet, *pos;
    int send_len;

    while (temp != NULL) {
        count++;
        temp = temp->next;
    }

    /* Set up packet */
    packet = malloc(sizeof(Header) + sizeof(uint32_t));
    ((Header *)packet)->seq_num = htonl(seq_num++);
    ((Header *)packet)->flag = REP_NUM_HANDLES;
    pos = packet + sizeof(uint32_t) + sizeof(uint8_t);
    *(uint32_t *)pos = count;

    /* Send it */
    if ((send_len = send(client_socket, packet, sizeof(Header) + 
                    sizeof(uint32_t), 0)) < 0) {
        perror("send");
        exit(-1);
    }

    free(packet);
}

/* Gets the num'th number handle, if it exists still */
char *get_handle(uint32_t num)
{
    Node *temp = head;
    int i = 0;

    for (i = 0; i < num; i++) {
        /* Handle doesn't exist */
        if (temp == NULL) {
            return NULL;
        }
        temp = temp->next;
    }

    return temp->handle;
}

/* Sends the request handle to the client */
void send_handle(int client_socket, void *packet)
{
    int send_len, sent;
    char *dest_handle;
    void *toSend, *pos;

    /* Check if handle exists still */
    packet += sizeof(uint8_t);
    dest_handle = get_handle(ntohl(*(uint32_t *)packet));
    
    /* If the handle no longer exists */
    if (dest_handle == NULL) {
        send_len = sizeof(Header);
        toSend = malloc(send_len);
        pos = toSend;
        *(uint32_t *)pos = htonl(seq_num++);
        pos += sizeof(uint32_t);
        *(uint8_t *)pos = REP_BAD_HANDLE;
    }
    /* Else, fill packet with handle info */
    else {
        send_len = sizeof(Header) + sizeof(uint8_t) + strlen(dest_handle);
        toSend = malloc(send_len);
        pos = toSend;
        *(uint32_t *)pos = htonl(seq_num++);
        pos += sizeof(uint32_t);
        *(uint8_t *)pos = REP_HANDLE;
        pos += sizeof(uint8_t);
        *(uint8_t *)pos = strlen(dest_handle);
        pos += sizeof(uint8_t);
        memcpy(pos, dest_handle, strlen(dest_handle));
    }

    /* Send it */
    if ((sent = send(client_socket, toSend, send_len, 0)) < 0) {
        perror("send");
        exit(-1);
    }

    free(toSend);
}

/* Handle a new message from a client */
void handle_message(int client_socket, void *buf, fd_set *readfds, 
        fd_set *master)
{
    int message_len;
    void *packet;

    /* now get the data on the client_socket */
    if ((message_len = recv(client_socket, buf, BUFF_SIZE, 0)) > 0) {
        packet = buf + sizeof(uint32_t);
        switch (*(uint8_t *)packet) {
            case INIT_PKT:
                printf("Initial packet received\n");
                init_check(buf, client_socket);
                break;
            case BCAST_PKT:
                printf("Broadcast packet received\n");
                broadcast_message(client_socket, buf, message_len, readfds);
                break;
            case MSG_PKT:
                printf("Message packet received\n");
                forward_message(buf, message_len); 
                break;
            case REQ_EXIT:
                printf("Client requesting exit\n");
                ack_exit(client_socket, buf, master);
                break;
            case REQ_NUM_HANDLES:
                printf("Client requesting handle count\n");
                send_handle_count(client_socket);
                break;
            case REQ_HANDLE:
                printf("Client requesting a handle\n");
                send_handle(client_socket, packet);
                break;
            default:
                printf("Unknown flag %d\n", *(uint8_t *)packet);
                break;
        }
    }

    if (message_len == 0) {
        printf("Terminating client...\n");
        remove_client(client_socket, master);
    }
    if (message_len < 0) {
        perror("recv call\n");
        exit(-1);
    }
}

/* Add a new client to the master list */
void add_client(int server_socket, fd_set *master)
{
    int client_socket;
    Node *temp, *new;

    if ((client_socket = accept(server_socket, (struct sockaddr*)0, 
                  (socklen_t *)0)) < 0) {
        perror("accept call");
        exit(-1);
    }
    
    /* Add to master set and create new node */
    new = calloc(1, sizeof(Node));
    FD_SET(client_socket, master);
    new->socket = client_socket;

    if ((temp = head) == NULL)
        head = new;
    else {
        while (temp->next != NULL)
            temp = temp->next;
        temp->next = new;
    }

    if (client_socket >= max)
        max = client_socket+1;
}

/* Remove a client from the master list and close its socket */
void remove_client(int client_socket, fd_set *master)
{
    Node *temp = head, *toFree;
    printf("Removing client from master and global list...\n");
    
    /* Nothing in list */
    if (temp == NULL) {
        printf("Client already deleted\n");
        return;
    }

    /* Client is head */
    if (temp != NULL && temp->socket == client_socket) {
        FD_CLR(client_socket, master);
        close(client_socket);
        head = temp->next;
        free(temp);
        printf("Client successfully deleted\n");
        return;
    }

    /* Check for client in list */
    while (temp != NULL) {
        if (temp->next != NULL && temp->next->socket == client_socket) {
            FD_CLR(client_socket, master);
            close(client_socket);
            toFree = temp->next;
            temp->next = toFree->next;
            free(toFree);
            printf("Client successfully deleted\n");
            return;
        }
        
        temp = temp->next;
    }
}

/* Completely removes any remaining clients and closes their sockets */
void clear_list(fd_set *master)
{
    Node *temp = head, *toFree;

    while (temp != NULL) {
        toFree = temp;
        temp = temp->next;
        remove_client(toFree->socket, master);
        free(toFree);
    }
}

/* Checks if the handle exists, returns its socket if it does, 
 * otherwise returns false */
int check_handle(char *handle)
{
    Node *temp = head;
    int ret = 0;

    while (temp != NULL) {
       ret = strcmp(handle, temp->handle);
       if (ret == 0) {
           return temp->socket;
       }
       temp = temp->next;
    }

    return 0;
}

/* Forwards a message from a client to another client */
void forward_message(void *packet, int send_len)
{
    char *dest_handle;
    uint8_t len;
    void *pos;
    
    pos = packet;
    packet += sizeof(uint32_t) + sizeof(uint8_t);

    /* Copy in dest handle */
    len = *(uint8_t *)packet;
    dest_handle = malloc(len+1);
    dest_handle[len] = '\0';
    packet += sizeof(uint8_t);
    memcpy(dest_handle, packet, len);
    packet += len;

    send_message(dest_handle, pos, send_len);
}

/* Broadcasts a message to every client */
void broadcast_message(int src_socket, void *packet, int send_len, 
        fd_set *readfds)
{
    int i, sent = 0;

    /* Send message to everyone but sender */
    for (i = 0; i < max; i++) {
        if (i != server_socket && i != src_socket) {
            if ((sent = send(i, packet, send_len, 0)) < 0) {
                printf("Message not sent\n");
            }
            else
                printf("Message sent on: %d\n", i);
        }
    }
}

/* Actually send a message to a client */
void send_message(char *dest_handle, void *packet, int send_len)
{
    int sent = 0, dest_socket;
    void *pos;
    
    pos = packet;

    /* Set to flag to change if necessary */
    pos += sizeof(uint32_t);
    /* look up if handle name is in use */
    if ((dest_socket = check_handle(dest_handle)) == 0) {
        printf("Error: Handle doesn't exist\n");
        *(uint8_t *)pos = BAD_DEST_HANDLE;
    }

    /* send message */
    if ((sent = send(dest_socket, packet, send_len, 0)) < 0) {
        perror("Message not sent\n");
    }
    free(dest_handle);
}
                
/* This function sets the server socket.  It lets the system
determine the port number.  The function returns the server
socket number and prints the port number to the screen.  */
int tcp_recv_setup()
{
    int server_socket = 0;
    struct sockaddr_in local;       // socket address for local side
    socklen_t len = sizeof(local);  // length of local address

    /* create the socket  */
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
      perror("socket call");
      exit(1);
    }

    local.sin_family = AF_INET;             // internet family
    local.sin_addr.s_addr = INADDR_ANY;     // wild card machine address
    local.sin_port = htons(0);              // let system choose the port

    /* bind the name (address) to a port */
    if (bind(server_socket, (struct sockaddr *) &local, sizeof(local)) < 0) {  
        perror("bind call");
        exit(-1);
    }
    
    /* get the port name and print it out */
    if (getsockname(server_socket, (struct sockaddr*)&local, &len) < 0) {
        perror("getsockname call");
        exit(-1);
    }

    if (listen(server_socket, 5) < 0) {
        perror("listen call");
        exit(-1);
    }

    printf("socket listening on port %d \n", ntohs(local.sin_port));
	        
    return server_socket;
}
