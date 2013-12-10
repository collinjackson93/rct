#include "Connection.h"
#include "EventLoop.h"
#include "Serializer.h"
#include "Messages.h"
#include "Timer.h"
#include <assert.h>

#include "Connection.h"

Connection::Connection()
    : mPendingRead(0), mPendingWrite(0), mSilent(false)
{
}

Connection::Connection(const SocketClient::SharedPtr &client)
    : mSocketClient(client), mPendingRead(0), mPendingWrite(0), mSilent(false)
{
    assert(client->isConnected());
    mSocketClient->disconnected().connect(std::bind(&Connection::onClientDisconnected, this, std::placeholders::_1));
    mSocketClient->readyRead().connect(std::bind(&Connection::onDataAvailable, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->bytesWritten().connect(std::bind(&Connection::onDataWritten, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->error().connect(std::bind(&Connection::onSocketError, this, std::placeholders::_1, std::placeholders::_2));
    EventLoop::eventLoop()->callLater(std::bind(&Connection::checkData, this));
    // mCheckDataTimer =
    //   EventLoop::eventLoop()->
    //   registerTimer([&](int){ this->checkData(); }, 1, Timer::SingleShot);
}

void Connection::checkData()
{
    if (!mSocketClient->buffer().isEmpty())
        onDataAvailable(mSocketClient, std::forward<Buffer>(mSocketClient->takeBuffer()));
}

#warning need to respect timeout
bool Connection::connectUnix(const Path &socketFile, int timeout)
{
    mSocketClient.reset(new SocketClient);
    mSocketClient->connected().connect(std::bind(&Connection::onClientConnected, this, std::placeholders::_1));
    mSocketClient->disconnected().connect(std::bind(&Connection::onClientDisconnected, this, std::placeholders::_1));
    mSocketClient->readyRead().connect(std::bind(&Connection::onDataAvailable, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->bytesWritten().connect(std::bind(&Connection::onDataWritten, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->error().connect(std::bind(&Connection::onSocketError, this, std::placeholders::_1, std::placeholders::_2));
    if (!mSocketClient->connect(socketFile)) {
        mSocketClient.reset();
        return false;
    }
    return true;
}

bool Connection::connectTcp(const String &host, uint16_t port, int timeout)
{
    mSocketClient.reset(new SocketClient);
    mSocketClient->connected().connect(std::bind(&Connection::onClientConnected, this, std::placeholders::_1));
    mSocketClient->disconnected().connect(std::bind(&Connection::onClientDisconnected, this, std::placeholders::_1));
    mSocketClient->readyRead().connect(std::bind(&Connection::onDataAvailable, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->bytesWritten().connect(std::bind(&Connection::onDataWritten, this, std::placeholders::_1, std::placeholders::_2));
    mSocketClient->error().connect(std::bind(&Connection::onSocketError, this, std::placeholders::_1, std::placeholders::_2));
    if (!mSocketClient->connect(host, port)) {
        mSocketClient.reset();
        return false;
    }
    return true;
}

bool Connection::sendData(uint8_t id, const String &message)
{
    // ::error() << getpid() << "sending message" << static_cast<int>(id) << message.size();
    if (!mSocketClient->isConnected()) {
        ::error("Trying to send message to unconnected client (%d)", id);
        return false;
    }

    String data;
    Serializer s(data);
    s << static_cast<uint32_t>(data.size()) << static_cast<int8_t>(Messages::Version) << id;
    if (!message.isEmpty())
        s.write(message.constData(), message.size());
    *reinterpret_cast<uint32_t*>(&data[0]) = data.size() - sizeof(uint32_t);
    mPendingWrite += data.size();
    return mSocketClient->write(data);
}

int Connection::pendingWrite() const
{
    return mPendingWrite;
}

static inline unsigned int bufferSize(const LinkedList<Buffer>& buffers)
{
    unsigned int sz = 0;
    for (const Buffer& buffer: buffers) {
        sz += buffer.size();
    }
    return sz;
}

static inline int bufferRead(LinkedList<Buffer>& buffers, char* out, unsigned int size)
{
    if (!size)
        return 0;
    unsigned int num = 0, rem = size, cur;
    LinkedList<Buffer>::iterator it = buffers.begin();
    while (it != buffers.end()) {
        cur = std::min(it->size(), rem);
        memcpy(out + num, it->data(), cur);
        rem -= cur;
        num += cur;
        if (cur == it->size()) {
            // we've read the entire buffer, remove it
            it = buffers.erase(it);
        } else {
            assert(!rem);
            assert(it->size() > cur);
            assert(cur > 0);
            // we need to shrink & memmove the front buffer at this point
            Buffer& front = *it;
            memmove(front.data(), front.data() + cur, front.size() - cur);
            front.resize(front.size() - cur);
        }
        if (!rem) {
            assert(num == size);
            return size;
        }
        assert(rem > 0);
    }
    return num;
}

void Connection::onDataAvailable(const SocketClient::SharedPtr&, Buffer&& buffer)
{
    while (true) {
        if (!mSocketClient->buffer().isEmpty())
            mBuffers.push_back(std::move(mSocketClient->takeBuffer()));
        unsigned int available = bufferSize(mBuffers);
        if (!available)
            break;
        if (!mPendingRead) {
            if (available < static_cast<int>(sizeof(uint32_t)))
                break;
            char buf[sizeof(uint32_t)];
            const int read = bufferRead(mBuffers, buf, 4);
            assert(read == 4);
            Deserializer strm(buf, read);
            strm >> mPendingRead;
            available -= 4;
        }
        assert(mPendingRead >= 0);
        if (available < static_cast<unsigned int>(mPendingRead))
            break;
        char buf[1024];
        char *buffer = buf;
        if (mPendingRead > static_cast<int>(sizeof(buf))) {
            buffer = new char[mPendingRead];
        }
        const int read = bufferRead(mBuffers, buffer, mPendingRead);
        assert(read == mPendingRead);
        mPendingRead = 0;
        Message *message = Messages::create(buffer, read);
        if (message) {
            if (message->messageId() == FinishMessage::MessageId) {
                mFinished(this);
            } else {
                newMessage()(message, this);
            }
            delete message;
        }
        if (buffer != buf)
            delete[] buffer;

        // mClient->dataAvailable().disconnect(this, &Connection::dataAvailable);
    }
}

void Connection::onDataWritten(const SocketClient::SharedPtr&, int bytes)
{
    assert(mPendingWrite >= bytes);
    mPendingWrite -= bytes;
    // ::error() << "wrote some bytes" << mPendingWrite << bytes;
    if (!mPendingWrite) {
        mSendFinished(this);
    }
}

void Connection::writeAsync(const String &out)
{
    EventLoop::eventLoop()->callLaterMove(std::bind((bool(Connection::*)(Message&&))&Connection::send, this, std::placeholders::_1), ResponseMessage(out));
}
