#define MSG_LEN 80
#define HANDLE_LEN 100
#define MESSAGE_LEN 1000
#define BUFF_SIZE 1024

// Client -> Server
#define INIT_PKT 0x1
#define BCAST_PKT 0x5
#define MSG_PKT 0x6
#define REQ_EXIT 0x8
#define REQ_NUM_HANDLES 0xa
#define REQ_HANDLE 0xc

// Server -> Client
#define GOOD_HANDLE 0x2
#define BAD_HANDLE 0x3
#define BAD_DEST_HANDLE 0x7
#define ACK_EXIT 0x9
#define REP_NUM_HANDLES 0xb
#define REP_HANDLE 0xd
#define REP_BAD_HANDLE 0xe

typedef struct {
    uint32_t seq_num;
    uint8_t flag;
} Header;
