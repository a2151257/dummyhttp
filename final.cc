#include <string>
#include <sstream>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <ev.h>

#define NTHREADS 4

using std::string;
using std::stringstream;

static const char code200[] = "HTTP/1.0 200 OK\r\n"
  "Content-length: %zd\r\n"
  "Content-Type: text/html\r\n\r\n";
static const char code404[] = "HTTP/1.0 404 NOT FOUND\r\nContent-Type: text/html\r\n\r\n";

char ip[17] = {0}; // TODO ipv6 ?
char port[6] = {0};
char directory[1024] = {0};
int global_acceptor_socket = 0;

struct WData {
  WData(int fd) : sockfd(fd), resfd(0), offset(0) {}
  
  int sockfd;
  int resfd;
  off_t offset;
  size_t to_send;
  string incoming;
};


static int sendall(int fd, const char *buf, int bytesleft)
{
  int total = 0; // how many bytes we've sent so far
  while(total < bytesleft) {
    int n = send(fd, buf+total, bytesleft, 0);
    if (n == -1) { 
      return -1; // TODO handle send errors?
    }
    total += n;
    bytesleft -= n;
  }
  return 0;
} 

static void write_cb(struct ev_loop *loop, ev_io* watcher, int) {
  WData* d = (WData*)(watcher->data);
  ssize_t s = sendfile(d->sockfd, d->resfd, &d->offset, d->to_send);
  if (s > 0) {
    d->to_send -= s;
  }
  if (d->to_send == 0) {
    ev_io_stop(loop, watcher);
    close(d->sockfd);
    close(d->resfd);
    delete d;
    free(watcher);
  }
}

static bool final_empty_line(const string& s) {
  size_t len = s.length();
  size_t f4 = s.rfind("\r\n\r\n");
  size_t f2 = s.rfind("\n\n");
  return (len - f2 == 2 || len - f4 == 4);
}

static void read_cb(struct ev_loop *loop, ev_io* watcher, int) {
  WData* d = (WData*)(watcher->data);
  char buf[1024];
  int r = recv(d->sockfd, buf, 1023, 0);
  if (r == 0) {
    ev_io_stop(loop, watcher);
    close(d->sockfd);
    delete d;
    free(watcher);
    return;
  }
  buf[r] = '\0';
  string s(buf);
  d->incoming += s;
  if (final_empty_line(d->incoming)) {
    ev_io_stop(loop, watcher);

    stringstream ss(d->incoming);
    string method, uri, protocol;
    ss >> method >> uri >> protocol;
    // TODO check method and protocol
    string path(directory);
    uri = uri.substr(0, uri.find('?')); // strip request parameters ('?' sign and all after it; ignoring them for now)
    path += uri;
    // now check if the requested resource is present and readable
    struct stat statbuf;
    stat(path.c_str(), &statbuf);
    if (S_ISREG(statbuf.st_mode) && ((d->resfd = open(path.c_str(), O_RDONLY)) != -1)) {
	d->to_send = statbuf.st_size;
	char header[sizeof(code200) + 100];
	sprintf(header, code200, d->to_send);
	sendall(d->sockfd, header, strlen(header));
	ev_io_init(watcher, write_cb, d->sockfd, EV_WRITE);
	ev_io_start(loop, watcher);
    } else {
	sendall(d->sockfd, code404, strlen(code404));
	close(d->sockfd);
	delete d;
	free(watcher);
    }
  }
}

static void accept_cb(struct ev_loop *loop, ev_io*, int) {
  int sockfd = accept4(global_acceptor_socket, NULL, NULL, SOCK_NONBLOCK);
  if (sockfd != -1) {
    ev_io* w = (ev_io*)malloc(sizeof(ev_io));
    WData* d = new WData(sockfd);
    w->data = d;
    ev_io_init(w, read_cb, sockfd, EV_READ);
    ev_io_start(loop, w);
  }
}

static void* worker_loop(void*) {
  struct ev_loop* loop = ev_loop_new(0);
  ev_io acceptor_watcher;
  ev_io_init(&acceptor_watcher, accept_cb, global_acceptor_socket, EV_READ);
  ev_io_start(loop, &acceptor_watcher);
  ev_run(loop, 0);
  // should never get here
  exit(EXIT_FAILURE);
  return NULL;
}

static void init_acceptor() {
  struct addrinfo hints, *servinfo;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  
  int rv;
  if ((rv = getaddrinfo(ip, port, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    exit(EXIT_FAILURE);
  }

  for(struct addrinfo *p = servinfo; p != NULL; p = p->ai_next) {
    if ((global_acceptor_socket = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK,
					 p->ai_protocol)) == -1) {
      perror("socket");
      continue;
    }

    int one = 1;
    if (setsockopt(global_acceptor_socket, SOL_SOCKET, SO_REUSEADDR, &one,
		   sizeof(int)) == -1) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }
    
    if (bind(global_acceptor_socket, p->ai_addr, p->ai_addrlen) == -1) {
      perror("bind");
      exit(EXIT_FAILURE);
    }

    if (listen(global_acceptor_socket, SOMAXCONN) == -1) {
      perror("listen");
      exit(EXIT_FAILURE);
    }

    fprintf(stderr, "waiting for connections...\n");
    freeaddrinfo(servinfo);
    return;
  }

  fprintf(stderr, "Failed to init global acceptor socket\n");
  exit(EXIT_FAILURE);
}

static void report_bad_params_and_exit(char* pname) {
  fprintf(stderr, "Usage: %s -h <ip> -p <port> -d <directory>\n",
	  pname);
  exit(EXIT_FAILURE);
}

static void daemonize() {
  pid_t pid, sid;
  pid = fork();
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS);
  }
  umask(0);
  sid = setsid();
  if (sid < 0) {
    exit(EXIT_FAILURE);
  }
  if ((chdir("/")) < 0) {
    exit(EXIT_FAILURE);
  }
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
}

int main(int argc, char *argv[])
{
  int opt;
  while ((opt = getopt(argc, argv, "h:p:d:")) != -1) {
    // TODO fail on too long arguments instead of truncating them
    switch (opt) {
    case 'h':
      strncpy(ip, optarg, 16);
      break;
    case 'p':
      strncpy(port, optarg, 5);
      break;
    case 'd':
      strncpy(directory, optarg, 1023);
      break;
    default: /* '?' */
      report_bad_params_and_exit(argv[0]);
    }
  }
  if (optind != 7) {
    report_bad_params_and_exit(argv[0]);
  }

  fprintf(stderr, "ip=%s; port=%s; directory=%s, optind=%d\n",
	  ip, port, directory, optind);

  daemonize();  

  init_acceptor();

  pthread_t tids[NTHREADS];
  for (pthread_t tid = 0; tid < NTHREADS; tid++) {
    pthread_create(tids+tid, NULL, worker_loop, NULL);
  }

  for (pthread_t tid = 0; tid < NTHREADS; tid++) {
    pthread_join(*(tids+tid), NULL);
  }
  // should never get here actually

  return 0;
}


