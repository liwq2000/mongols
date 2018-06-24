#include <fcntl.h>          
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signal.h>

#include <cstring>         
#include <cstdlib> 


#include <string>
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>


#include "tcp_server.hpp"



namespace mongols {

    bool tcp_server::done = false;

    void tcp_server::signal_normal_cb(int sig) {
        switch (sig) {
            case SIGTERM:
            case SIGHUP:
            case SIGQUIT:
            case SIGINT:
                tcp_server::done = true;
                break;
        }
    }

    tcp_server::tcp_server(const std::string& host
            , int port
            , int timeout
            , size_t buffer_size
            , size_t thread_size
            , int max_event_size) :
    epoll(max_event_size, -1)
    , host(host), port(port), listenfd(0), timeout(timeout), serveraddr()
    , buffer_size(buffer_size), clients(), main_mtx()
    , work_pool(thread_size), clients_pool(std::thread::hardware_concurrency()) {

    }

    tcp_server::~tcp_server() {
    }

    void tcp_server::run(const std::function<std::pair<std::string, bool>(const std::string&) >& g) {

        int connfd;

        this->listenfd = socket(AF_INET, SOCK_STREAM, 0);

        int on = 1;
        setsockopt(this->listenfd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof (on));

        struct timeval send_timeout, recv_timeout;
        send_timeout.tv_sec = this->timeout;
        send_timeout.tv_usec = 0;
        setsockopt(this->listenfd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof (send_timeout));

        recv_timeout.tv_sec = this->timeout;
        recv_timeout.tv_usec = 0;
        setsockopt(this->listenfd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof (recv_timeout));



        bzero(&this->serveraddr, sizeof (this->serveraddr));
        this->serveraddr.sin_family = AF_INET;
        inet_aton(this->host.c_str(), &serveraddr.sin_addr);
        this->serveraddr.sin_port = htons(this->port);
        bind(this->listenfd, (struct sockaddr*) & this->serveraddr, sizeof (this->serveraddr));

        this->setnonblocking(this->listenfd);

        if (!this->epoll.is_ready()) {
            perror("epoll error");
            return;
        }
        this->epoll.add(this->listenfd, EPOLLIN | EPOLLET);


        listen(this->listenfd, 10);


        std::function<bool(int, bool) > client_work = [&](int fd, bool b) {
            if (fd > 0) {
                if (b) {
                    std::lock_guard<std::mutex> lk(this->main_mtx);
                    this->clients.insert(std::make_pair(fd, fd));
                } else if (this->clients.find(fd) != clients.end()) {
                    std::lock_guard<std::mutex> lk(this->main_mtx);
                    this->clients.erase(fd);
                }
            }
            return fd > 0 ? false : true;
        };


        std::function<bool(int) > work = [&](int fd) {
            if (fd > 0) {
                char buffer[this->buffer_size] = {0};
                ssize_t ret = recv(fd, buffer, this->buffer_size, MSG_DONTWAIT);
                if (ret >= 0) {
                    std::string input = std::move(std::string(buffer, ret));
                    std::pair < std::string, bool> output = std::move(g(input));
                    if (send(fd, output.first.c_str(), output.first.size(), MSG_DONTWAIT) < 0 || output.second) {
                        goto ev_error;
                    }
                } else {
ev_error:
                    close(fd);
                    this->clients_pool.submit(std::bind(client_work, fd, false));
                }
            }
            return fd > 0 ? false : true;
        };


        std::function<void(struct epoll_event*) > main_loop = [&](struct epoll_event * event) {
            if ((event->events & EPOLLERR) ||
                    (event->events & EPOLLHUP) ||
                    (!(event->events & EPOLLIN))) {
                close(event->data.fd);
            } else if (event->events & EPOLLRDHUP) {
                close(event->data.fd);
            } else if (event->data.fd == this->listenfd) {
                while (!tcp_server::done) {
                    struct sockaddr_in clientaddr;
                    socklen_t clilen;
                    connfd = accept(listenfd, (struct sockaddr*) &clientaddr, &clilen);
                    if (connfd > 0) {
                        setnonblocking(connfd);
                        this->epoll.add(connfd, EPOLLIN | EPOLLRDHUP | EPOLLET);

                        this->clients_pool.submit(std::bind(client_work, connfd, true));

                        if (this->work_pool.size() > 0) {
                            std::this_thread::yield();
                        }
                    } else {
                        break;
                    }
                }
            } else {
                if (this->work_pool.size() > 0) {
                    this->work_pool.submit(std::bind(work, int(event->data.fd)));
                    std::this_thread::yield();
                } else {
                    work(event->data.fd);
                }
            }
        };


        signal(SIGHUP, tcp_server::signal_normal_cb);
        signal(SIGTERM, tcp_server::signal_normal_cb);
        signal(SIGINT, tcp_server::signal_normal_cb);
        signal(SIGQUIT, tcp_server::signal_normal_cb);

        while (!tcp_server::done) {
            this->epoll.loop(main_loop);
        }

        for (size_t i = 0; i<this->work_pool.size(); ++i) {
            this->work_pool.submit(std::bind(work, -1));
            std::this_thread::yield();
            usleep(100);
        }
        for (size_t i = 0; i<this->clients_pool.size(); ++i) {
            this->clients_pool.submit(std::bind(client_work, -1, false));
            std::this_thread::yield();
            usleep(100);
        }

    }

    void tcp_server::setnonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }



}