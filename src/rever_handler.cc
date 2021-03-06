#include "rever_handler.h"
#include "rever_epoll.h"
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <sstream>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// after add glog, discard DEBUGLOG
#define DEBUG 0
#define DEBUGLOG(x, param)                                                     \
  do {                                                                         \
    if (DEBUG) {                                                               \
      printf(x, param);                                                        \
      fflush(stdout);                                                          \
    }                                                                          \
  } while (0);

void Handler::ReadAll() {
  char c = '\0';
  while (true) {
    int n = recv(fd_, &c, 1, 0);
    if (n > 0)
      buf_.push_back(c);
    else if (n = -1 && errno == EAGAIN) {
      LOG(INFO) << "ET mode receive end";
      break;
    } else if (n == 0) {
      LOG(INFO) << fd_ << " disconnected";
      close(fd_);
      break;
    }
  }
}

Handler::Handler(int _fd) : fd_(_fd), 
                            buf_(""), 
                            query_string_("") {}

Handler::~Handler() {
  // if (fd_ != -1) {
  //   // printf("client fd: %d disconnected\n", fd_);
  //   close(fd_);
  // }
}

int Handler::GetLine() {
  buf_.clear();
  char c = '\0';
  while (c != '\n') {
    int n = recv(fd_, &c, 1, 0);
    if (n > 0) {
      if (c == '\r') {
        n = recv(fd_, &c, 1, MSG_PEEK);
        if (n > 0 && c == '\n')
          recv(fd_, &c, 1, 0);
        else
          c = '\n';
      }
      buf_.push_back(c);
    } else
      c = '\n';
  }
  return buf_.size();
}

int Handler::GetRequestHeader() {
  int n = this->GetLine();
  // ERRIF(n == 0, "Empty Message: ");
  if (n == 0)
    return 0;
  ERRIF(n == -1, "Error Connection: ");
  std::istringstream is(buf_);
  is >> method_;
  is >> url_;
  is >> protocol_;
  if (method_ == "GET") {
    int pos = -1;
    if ((pos = url_.find('?')) != std::string::npos) {
      query_string_ = url_.substr(pos);
      url_.erase(pos);
      cgi_ = true;
    }
  }
  LOG(INFO) << method_ << " " << url_ << " " << protocol_;
  return n;
}

/* ???????????????GET AND POST */
bool Handler::IsImpl() {
  if (method_ != "GET" && method_ != "POST") {
    Unimplemented(fd_);
    return false;
  }
  return true;
}

/* ??????url */
void Handler::FileProcess() {
  url_ = "../htdocs" + url_;
  /* ?????????/???????????????????????????????????????index.html */
  if (url_[url_.size() - 1] == '/') {
    url_ += "index.html";
  }
  struct stat st;
  if (stat(url_.c_str(), &st) == -1) {
    /*
     * -1?????????????????????
     * ??????TCP?????????????????????????????????(??????)???
     * ?????????????????????????????????
     */
    ReadAll();
    NotFound(fd_);
  } else {
    /* ?????? */
    if (S_ISDIR(st.st_mode)) {
      url_ += "/index.html";
    }
    if ((st.st_mode & S_IXUSR) || (st.st_mode & S_IXGRP) ||
        (st.st_mode & S_IXOTH)) {
      /* ?????????????????? */
      cgi_ = true;
    }
    if (cgi_)
      Execute();
    else
      SendFile();
  }
}

void Handler::SendFile() {
  FILE *fp = nullptr;
  int n = 1;
  // while (n > 0 && buf_ != "\n") {
  //   /* ????????????????????????????????? */
  //   n = GetLine();
  // }
  ReadAll();
  fp = fopen(url_.c_str(), "r");
  if (fp == nullptr) {
    NotFound(fd_);
  } else {
    SendHeader(fd_);
    Cat(fp);
  }
  fclose(fp);
}

void Handler::Cat(FILE *fp) {
  char buf_[1024];
  fgets(buf_, sizeof(buf_), fp);
  while (!feof(fp)) {
    send(this->fd_, buf_, strlen(buf_), 0);
    fgets(buf_, sizeof(buf_), fp);
  }
}

void Handler::Execute() {
  /* ??????????????????????????????????????????thread??? */
  // ?????????????????? broken pipe
  int cgi_output[2];
  int cgi_input[2];
  pid_t pid;
  int status;
  int content_length = 0;
  bool flag = true;
  if (method_ == "GET") {
    int n = 1;
    // while (n > 0 && buf_ != "\n")
    //   n = GetLine();
    ReadAll();
  } else {
    int n = 1;
    while (n > 0) {
      n = GetLine();
      if (buf_ == "\n")
        break;
      if (flag && buf_.size() >= 15 && buf_.substr(0, 15) == "Content-Length:") {
        content_length = stoi(buf_.substr(16));
        flag = false; /* ???????????????????????????????????????????????????????????? */
      }
    }
    if (content_length == -1) {
      BadRequest(fd_);
      return;
    }
  }
  char temp[128];
  sprintf(temp, "HTTP/1.0 200 OK\r\n");
  send(fd_, temp, strlen(temp), 0);
  if (pipe(cgi_output) < 0) {
    CannotExecute(fd_);
    return;
  }
  if (pipe(cgi_input) < 0) {
    CannotExecute(fd_);
    return;
  }
  if ((pid = fork()) < 0) {
    CannotExecute(fd_);
    return;
  }
  if (pid == 0) {
    char method_env[255];
    char query_env[255];
    char length_env[255];
    close(cgi_output[0]);
    close(cgi_input[1]);
    dup2(cgi_output[1], 1); /* ?????????????????????????????????????????? */
    dup2(cgi_input[0], 0);  /* ????????????????????????????????????????????? */
    sprintf(method_env, "REQUEST_METHOD=%s", method_.c_str());
    putenv(method_env);
    if (this->method_ == "GET") {
      sprintf(query_env, "query_string_=%s", query_string_.c_str());
      putenv(query_env);
    } else {
      /* POST */
      sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
      putenv(length_env);
    }
    execl(url_.c_str(), query_string_.c_str(), NULL);
    exit(0);
  } else {
    close(cgi_output[1]);
    close(cgi_input[0]);
    char c = '\0';
    if (method_ == "POST") {
      for (int i = 0; i < content_length; ++i) {
        recv(fd_, &c, 1, 0);
        write(cgi_input[1], &c, 1);
      }
    }
    while (read(cgi_output[0], &c, 1) > 0) {
      send(fd_, &c, 1, 0);
    }
    close(cgi_output[0]);
    close(cgi_input[1]);
    waitpid(pid, &status, 0);
  }
}

int Handler::Peek() {
  char c = '\0';
  int n = recv(fd_, &c, 1, MSG_PEEK);
  LOG(INFO) << n << " bytes peeked";
  return n;
}

void Handler::Start() {
  // ?????????????????????(??????????????????)
  if (Peek() <= 0)
    return;
  GetRequestHeader();
  if (!IsImpl()) {
    LOG(INFO) << "method not implemented";
    return;
  }
  FileProcess();
  /* Handler????????????????????? */
}