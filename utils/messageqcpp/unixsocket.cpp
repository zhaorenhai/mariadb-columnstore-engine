#include "unixsocket.h"
#include "socketclosed.h"

#include <poll.h>

using namespace std;

namespace messageqcpp
{

UnixSocket::UnixSocket()
{
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
}

UnixSocket::UnixSocket(const UnixSocket &rhs)
{
    parms = rhs.parms;
    memcpy(&sun, &rhs.sun, sizeof(sun));
}

UnixSocket::~UnixSocket()
{
    if (parms.sd() >= 0)
        ::close(parms.sd());
}

UnixSocket & UnixSocket::operator=(const UnixSocket &rhs)
{
    if (parms.sd() >= 0)
        ::close(parms.sd());
    parms = rhs.parms;
    memcpy(&sun, &rhs.sun, sizeof(sun));
    return *this;
}

void UnixSocket::open()
{
    if (parms.sd() >= 0)
        ::close(parms.sd());
    parms.sd(::socket(AF_UNIX, SOCK_STREAM, 0));
}

void UnixSocket::connect(const sockaddr *serv_addr)
{
    if (parms.sd() < 0)
        throw runtime_error("UnixSocket::connect(): no socket");
    sockaddr_in *sin = (sockaddr_in *) serv_addr;
    uint16_t port = ntohs(sin->sin_port);

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    sprintf(&sun.sun_path[1], "%s%d", &prefix[1], port);
    int err = ::connect(parms.sd(), (sockaddr *) &sun, sizeof(sun));
    if (err < 0)
        throw runtime_error(string("UnixSocket::connect(): failed to connect on ") + &sun.sun_path[1]);
}

const IOSocket UnixSocket::accept(const struct timespec *timeout)
{
    IOSocket ret(new UnixSocket());

    if (timeout != NULL)
    {
        struct pollfd pfd[1];
        pfd[0].fd = socketParms().sd();
        pfd[0].events = POLLIN;

        if (timeout != 0)
        {
            int msecs = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;

            if (poll(pfd, 1, msecs) != 1 || (pfd[0].revents & POLLIN) == 0 ||
                    pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
                return ret;
        }
    }

    struct sockaddr remote;
    socklen_t sl = sizeof(remote);
    int err, clientfd;

    do
    {
        clientfd = ::accept(parms.sd(), &remote, &sl);
        err = errno;
    } while (clientfd < 0 && (err == EINTR || err == ERESTART ||
        err == ECONNABORTED));

    if (clientfd < 0)
    {
        char buf[80], *ctmp;
        ctmp = strerror_r(err, buf, 80);
        string msg = string("UnixSocket::accept(): error: ") + ctmp;
        throw runtime_error(msg);
    }

    SocketParms sp = ret.socketParms();
    sp.sd(clientfd);
    ret.socketParms(sp);
    ret.sa(&remote);
    return ret;
}

/* This currently assumes the remote is writing complete msgs, not bothering
with message deliminters, or comprehensive timeout checking yet. */
const SBS UnixSocket::read(const timespec *timeout, bool *isTimeout, Stats *stats) const
{
    SBS ret;
    ssize_t err;
    uint64_t msecs;
    uint32_t msglen;
    int l_errno;

    pollfd pfd[1];
    pfd[0].fd = parms.sd();
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;

    if (timeout)
    {
        msecs = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
        err = poll(pfd, 1, msecs);
        if (err < 0 || pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            l_errno = errno;
            ostringstream oss;
            char buf[80];
            oss << "UnixSocket::read: I/O error1: " << strerror_r(l_errno, buf, 80);
            throw runtime_error(oss.str());
        }
        if (err == 0)  // timeout
        {
            if (isTimeout)
                *isTimeout = true;
            return SBS(new ByteStream(0));
        }
    }

    while (1)
    {
        err = ::read(parms.sd(), &msglen, sizeof(msglen));
        if (err < 0)
        {
            if (errno == EINTR)
                continue;
            else
            {
                l_errno = errno;
                ostringstream oss;
                char buf[80];
                oss << "UnixSocket::read: I/O error2: " << strerror_r(l_errno, buf, 80);
                throw runtime_error(oss.str());
            }
        }
        else if (err == 0)
        {
            if (timeout)
                throw SocketClosed("UnixSocket::read(): Remote is closed");
            else
                return SBS(new ByteStream(0));
        }
        else if (err != sizeof(msglen))
            throw runtime_error("fixme, unixsocket failed to read 4 bytes at once");
    }

    if (stats)
        stats->dataRecvd(sizeof(msglen));

    ret.reset(new ByteStream(msglen));
    uint8_t *buf = ret->getInputPtr();
    size_t readSoFar = 0;

    while (readSoFar < msglen)
    {
        err = ::read(parms.sd(), &buf[readSoFar], msglen - readSoFar);
        if (err < 0)
        {
            if (errno == EINTR)
                continue;
            else
            {
                l_errno = errno;
                ostringstream oss;
                char buf[80];
                oss << "UnixSocket::read: I/O error3: " << strerror_r(l_errno, buf, 80);
                throw runtime_error(oss.str());
            }
        }
        else if (err == 0)
        {
            if (stats)
                stats->dataRecvd(readSoFar);
            if (timeout)
                throw SocketClosed("UnixSocket::read: Remote is closed");
            else
                return SBS(new ByteStream(0));
        }
        else
            readSoFar += err;
    }
    if (stats)
        stats->dataRecvd(msglen);
    ret->advanceInputPtr(msglen);
    return ret;
}

void UnixSocket::write_raw(const ByteStream &msg, Stats *stats) const
{
    size_t toSend = msg.length(), sent = 0;
    const uint8_t *buf = msg.buf();
    ssize_t err;
    int l_errno;
    char errbuf[80];

    while (sent < toSend)
    {
        err = ::write(parms.sd(), &buf[sent], toSend - sent);
        if (err < 0)
        {
            if (errno == EINTR)
                continue;
            l_errno = errno;
            if (stats)
                stats->dataSent(sent);
            throw runtime_error(string("UnixSocket:write_raw: got ") +
                strerror_r(l_errno, errbuf, 80));
        }
        sent += err;
    }
    if (stats)
        stats->dataSent(sent);
}

void UnixSocket::write(SBS msg, Stats *stats)
{
    write(*msg, stats);
}

void UnixSocket::write(const ByteStream &msg, Stats *stats)
{
    ssize_t err;
    uint32_t size = msg.length();
    char buf[80];

    while (err != sizeof(size))
    {
        err = ::write(parms.sd(), &size, sizeof(size));
        if (err < 0)   // todo, robustify...
        {
            if (errno == EINTR)
                continue;
            int l_errno = errno;
            throw runtime_error(string("UnixSocket:write: got ") +
                strerror_r(l_errno, buf, 80));
        }
    }

    if (stats)
        stats->dataSent(sizeof(size));
    write_raw(msg, stats);
}

const bool UnixSocket::isOpen() const
{
    return parms.isOpen();
}

const SocketParms UnixSocket::socketParms() const
{
    return parms;
}

void UnixSocket::sa(const sockaddr *other)
{
    // I think this will always be AF_UNIX..?
    if (other->sa_family == AF_UNIX)
        memcpy(&sun, other, sizeof(sun));
    else if (other->sa_family == PF_INET || other->sa_family == AF_INET)
    {
        cout << "Not an AF_UNIX scoket" << endl;
        sockaddr_in *sin = (sockaddr_in *) other;
        uint16_t port = ntohs(sin->sin_port);
        sun.sun_family = AF_UNIX;
        sprintf(&sun.sun_path[1], "%s%d", &prefix[1], port);
    }
}

Socket * UnixSocket::clone() const
{
    return new UnixSocket(*this);
}

void UnixSocket::connectionTimeout(const struct ::timespec *)
{
    // TODO: not implemented yet
}

void UnixSocket::syncProto(bool use)
{
    // doubt this needs to be implemented for unix sockets
}

const int UnixSocket::getConnectionNum() const
{
    return parms.sd();
}

const string UnixSocket::addr2String() const
{
    return string("UnixSocket on ") + &sun.sun_path[1];
}

const bool UnixSocket::isSameAddr(const Socket *s) const
{
    const UnixSocket *other = dynamic_cast<const UnixSocket *>(s);
    if (!other)
        return false;
    else
        return strncmp(&sun.sun_path[1], &(other->sun.sun_path[1]),
            sizeof(sun.sun_path) - 1);
}

bool UnixSocket::isConnected() const
{
    // lifted from InetStreamSocket::isConnected()

    int error = 0;
    socklen_t len = sizeof(error);
    int retval = getsockopt(parms.sd(), SOL_SOCKET, SO_ERROR, &error, &len);

    if (error || retval)
        return false;

    struct pollfd pfd[1];
    pfd[0].fd = parms.sd();
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;

    error = ::poll(pfd, 1, 0);

    if ((error < 0) || (pfd[0].revents & (POLLHUP | POLLNVAL | POLLERR)))
        return false;

    return true;
}

bool UnixSocket::hasData() const
{
    char buf;
    int err = ::recv(parms.sd(), &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    return (err == 1);
}

}