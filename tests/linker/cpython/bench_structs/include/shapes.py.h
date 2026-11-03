/* Benchmark shapes modelled on a server's request boundary: a fixed-capacity
 * byte array plus a length in both directions, a small nested struct, and a
 * flat struct of strings and ints as an argument. */
#define CHUNK 65536
#define HEAD_MAX 32768

struct chunk {
    unsigned char data[CHUNK];
    int len;
};

struct point {
    int x;
    int y;
};

struct answer {
    int kind;
    int status;
    struct point origin;
    int headers_len;
    unsigned char headers[HEAD_MAX];
};

struct meta {
    int target;
    int tls;
    const char *method;
    const char *request_uri;
    const char *path;
    const char *query;
    const char *version;
    const char *remote;
    const char *local_addr;
    const char *local_port;
    const char *server_name;
};

extern int take(int req, struct chunk c);
extern struct chunk give(int req, int n);
extern struct answer ask(int req, struct meta m);
