
typedef struct hostbridge_receiver_entry_s {
    alt_u8 channel;
    alt_u8 *(*receiver)(const alt_u8 *ptr, int len, int *send_len);
    struct hostbridge_receiver_entry_s *next;
} hostbridge_receiver_entry;
