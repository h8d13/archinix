#include "nix/util/serialise.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/signals.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/util.hh"

#include <condition_variable>
#include <cstring>
#include <cerrno>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/coroutine2/coroutine.hpp>
#include <boost/coroutine2/protected_fixedsize_stack.hpp>

#  include <poll.h>

namespace nix {

void SerialisationError::anchor() {}

void Sink::anchor() {}

void BufferedSink::anchor() {}

void FdSink::anchor() {}

void StringSink::anchor() {}

void TeeSink::anchor() {}

void FinishSink::anchor() {}

void LambdaSink::anchor() {}

void NullSink::anchor() {}

void FramedSink::anchor() {}

void Source::anchor() {}

void BufferedSource::anchor() {}

void RestartableSource::anchor() {}

void FdSource::anchor() {}

void TeeSource::anchor() {}

void StringSource::anchor() {}

void LambdaSource::anchor() {}

void FramedSource::anchor() {}

void LengthSource::anchor() {}

void EnsureRead::anchor() {}

void ChainSource::anchor() {}

void SizedSource::anchor() {}

void BufferedSink::operator()(std::string_view data)
{
    if (!buffer)
        buffer = decltype(buffer)(new char[bufSize]);

    while (!data.empty()) {
        /* Optimisation: bypass the buffer if the data exceeds the
           buffer size. */
        if (data.size() >= bufSize - bufPos) {
            flush();
            writeUnbuffered(data);
            break;
        }
        /* Otherwise, copy the bytes to the buffer.  Flush the buffer
           when it's full. */
        size_t n = data.size() > bufSize - bufPos ? bufSize - bufPos : data.size();
        memcpy(buffer.get() + bufPos, data.data(), n);
        data.remove_prefix(n);
        bufPos += n;
        if (bufPos == bufSize)
            flush();
    }
}

void BufferedSink::flush()
{
    if (bufPos == 0)
        return;
    size_t n = bufPos;
    bufPos = 0; // don't trigger the assert() in ~BufferedSink()
    writeUnbuffered({buffer.get(), n});
}

FdSink::~FdSink()
{
    try {
        flush();
    } catch (...) {
        ignoreExceptionInDestructor();
    }
}

void FdSink::writeUnbuffered(std::string_view data)
{
    written += data.size();
    try {
        writeFull(fd, data);
    } catch (SystemError & e) {
        _good = false;
        throw;
    }
}

bool FdSink::good()
{
    return _good;
}

void Source::operator()(char * data, size_t len)
{
    while (len) {
        size_t n = read(data, len);
        data += n;
        len -= n;
    }
}

void Source::operator()(std::string_view data)
{
    (*this)((char *) data.data(), data.size());
}

void Source::drainInto(Sink & sink)
{
    std::array<char, 8192> buf;
    while (true) {
        try {
            auto n = read(buf.data(), buf.size());
            sink({buf.data(), n});
        } catch (EndOfFile &) {
            break;
        }
    }
}

void Source::drainInto(Sink & sink, uint64_t len)
{
    std::array<char, 65536> buf;
    while (len) {
        checkInterrupt();
        // Until std::saturate_cast is available (C++26)
        auto lenTrunc = static_cast<size_t>(std::min<uint64_t>(len, std::numeric_limits<size_t>::max()));
        auto n = read(buf.data(), std::min(lenTrunc, buf.size()));
        sink({buf.data(), n});
        assert(n <= len);
        len -= n;
    }
}

void Source::skip(size_t len)
{
    std::array<char, 8192> buf;
    while (len) {
        auto n = read(buf.data(), std::min(len, buf.size()));
        assert(n <= len);
        len -= n;
    }
}

size_t BufferedSource::read(char * data, size_t len)
{
    if (!buffer)
        buffer = std::make_unique_for_overwrite<char[]>(bufSize);

    if (!bufPosIn)
        bufPosIn = readUnbuffered(buffer.get(), bufSize);

    /* Copy out the data in the buffer. */
    auto n = std::min(len, bufPosIn - bufPosOut);
    memcpy(data, buffer.get() + bufPosOut, n);
    bufPosOut += n;
    if (bufPosIn == bufPosOut)
        bufPosIn = bufPosOut = 0;
    return n;
}

size_t FdSource::readUnbuffered(char * data, size_t len)
{
    auto n = nix::read(fd, {reinterpret_cast<std::byte *>(data), len});
    if (n == 0) {
        _good = false;
        throw EndOfFile(std::string(*endOfFileError));
    }
    read += n;
    return n;
}

bool FdSource::good()
{
    return _good;
}

void FdSource::restart()
{
    if (!isSeekable)
        throw Error("can't seek to the start of a file");
    buffer.reset();
    read = bufPosIn = bufPosOut = 0;
    if (lseek(fd, 0, SEEK_SET) == -1)
        throw SysError("seeking to the start of a file");
}

void FdSource::skip(size_t len)
{
    /* Discard data in the buffer. */
    if (len && buffer && bufPosIn - bufPosOut) {
        if (len >= bufPosIn - bufPosOut) {
            len -= bufPosIn - bufPosOut;
            bufPosIn = bufPosOut = 0;
        } else {
            bufPosOut += len;
            len = 0;
        }
    }

    /* If we can, seek forward in the file to skip the rest. */
    if (isSeekable && len) {
        if (len > static_cast<size_t>(std::numeric_limits<off_t>::max()))
            throw Error("cannot skip %d bytes: exceeds maximum file offset", len);
        if (lseek(fd, len, SEEK_CUR) == -1) {
            if (errno == ESPIPE)
                isSeekable = false;
            else
                throw SysError("seeking forward in file");
        } else {
            read += len;
            return;
        }
    }

    /* Otherwise, skip by reading. */
    if (len)
        BufferedSource::skip(len);
}

size_t StringSource::read(char * data, size_t len)
{
    if (pos == s.size())
        throw EndOfFile("end of string reached");
    size_t n = s.copy(data, len, pos);
    pos += n;
    return n;
}

void StringSource::skip(size_t len)
{
    const size_t remain = s.size() - pos;
    if (len > remain) {
        pos = s.size();
        throw EndOfFile("end of string reached");
    }
    pos += len;
}

/* 512KiB is a conservative estimate for deeply nested NARs, which are limited
   to 64 levels. We also tend to allocate rather large buffers on the stack, so
   we should leave plenty of headroom. Note that no evaluation is supposed to
   happen on sourceToSink/sinkToSource coroutine stacks (for Boehm GC reasons),
   which requires much more stack space. */
static constexpr size_t defaultCoroutineStackSize = 512 * 1024;

/* Chunks in flight between sourceToSink's two threads. At the 32 KiB
   BufferedSink chunk this is ~2 MiB of slack, enough to ride out a
   slow read or a slow write without either side idling, and bounded so
   a fast producer cannot balloon memory. */
static constexpr size_t sourceToSinkQueued = 64;

std::unique_ptr<FinishSink> sourceToSink(fun<void(Source &)> reader)
{
    /* Buffered for the same reason sinkToSource buffers its side:
       dumpPath emits the NAR as a stream of tiny tokens ("(", "type",
       "regular", each length word, each padding run). Unbuffered, every
       one of them resumes the coroutine and arrives at the consumer as
       its own read(), which downstream becomes a queue push per token
       into each async hash thread: a mutex, a notify and an allocation
       apiece. Measured on a 552 MiB tree: 940k futex calls, gone once
       the tokens coalesce. */
    /* A coroutine is not a thread: producer and consumer alternate on
       one OS stack, so the source reads and the destination writes of
       an import never overlap. On a warm 552 MiB tree that costs
       little (the reads are memcpy out of page cache), but on a
       multi-GiB tree read cold off the disk -- a box committing hours
       after boot, which is the shape that actually ships -- the whole
       import runs at read latency with the writer idle beside it, and
       then at write bandwidth with the reader idle. Measured on a
       9.3 GiB desktop tree: 11.5 s of source reads and ~9 s of writes
       that had to happen one after the other.

       So the boundary is a real thread with a bounded ring of buffers
       between the two sides, the same shape as the async hashers in
       libstore. The buffers are recycled rather than allocated per
       chunk: at 32 KiB a piece a multi-GiB import is hundreds of
       thousands of them. */
    struct SourceToSink : BufferedSink, FinishSink
    {
        /* both bases pin the vtable with their own anchor override, so
           this one has to name the final overrider */
        void anchor() override {}

        fun<void(Source &)> reader;

        std::thread worker;
        std::mutex mtx;
        std::condition_variable cvFull, cvFree;
        std::vector<std::string> full, free_;
        std::string producing, consuming;
        size_t consumed = 0;
        bool closed = false, quit = false;
        std::exception_ptr failure;

        SourceToSink(fun<void(Source &)> reader)
            : reader(reader)
        {
        }

        /* Consumer side: hand the reader the next chunk, blocking
           until one exists. EndOfFile once the producer has finished
           and the ring is drained, which is what the coroutine's
           "coroutine has finished" throw signalled. */
        size_t pull(char * out, size_t out_len)
        {
            if (consumed == consuming.size()) {
                std::unique_lock lk(mtx);
                /* hand the spent buffer back for reuse */
                if (!consuming.empty()) {
                    consuming.clear();
                    free_.push_back(std::move(consuming));
                }
                cvFull.wait(lk, [&] { return closed || !full.empty(); });
                if (full.empty())
                    throw EndOfFile("source-to-sink stream has finished");
                consuming = std::move(full.front());
                full.erase(full.begin());
                consumed = 0;
                /* it is taking a chunk out of a *full* ring that lets
                   the producer move again, not recycling the buffer */
                if (full.size() == sourceToSinkQueued - 1)
                    cvFree.notify_one();
            }
            size_t n = std::min(out_len, consuming.size() - consumed);
            memcpy(out, consuming.data() + consumed, n);
            consumed += n;
            return n;
        }

        void start()
        {
            worker = std::thread([this] {
                try {
                    LambdaSource source([this](char * out, size_t out_len) { return pull(out, out_len); });
                    reader(source);
                } catch (...) {
                    failure = std::current_exception();
                }
                /* whatever happened, stop the producer blocking on a
                   ring nobody will drain again */
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    quit = true;
                }
                cvFree.notify_one();
            });
        }

        void writeUnbuffered(std::string_view in) override
        {
            if (in.empty())
                return;
            if (!worker.joinable())
                start();

            producing.assign(in);

            std::unique_lock lk(mtx);
            cvFree.wait(lk, [&] { return quit || full.size() < sourceToSinkQueued; });
            if (quit) {
                /* the reader is gone; surface its failure here, the way
                   resuming a throwing coroutine used to */
                lk.unlock();
                stop();
                return;
            }
            full.push_back(std::move(producing));
            if (!free_.empty()) {
                producing = std::move(free_.back());
                free_.pop_back();
            } else
                producing.clear();
            bool wake = full.size() == 1;
            lk.unlock();
            if (wake)
                cvFull.notify_one();
        }

        /* Join the reader and re-throw whatever it raised. Idempotent:
           finish() and ~SourceToSink both call it. */
        void stop()
        {
            if (!worker.joinable())
                return;
            {
                std::lock_guard<std::mutex> lk(mtx);
                closed = true;
            }
            cvFull.notify_one();
            worker.join();
            if (failure) {
                auto e = failure;
                failure = nullptr;
                std::rethrow_exception(e);
            }
        }

        void finish() override
        {
            /* the tail of the NAR is still in the buffer; the reader
               must see it before being told the stream ended */
            flush();
            stop();
        }

        ~SourceToSink()
        {
            try {
                stop();
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        }
    };

    return std::make_unique<SourceToSink>(reader);
}

std::unique_ptr<Source> sinkToSource(fun<void(Sink &)> writer, fun<void()> eof)
{
    struct SinkToSource : Source
    {
        typedef boost::coroutines2::coroutine<std::string_view> coro_t;

        fun<void(Sink &)> writer;
        fun<void()> eof;
        std::optional<coro_t::pull_type> coro;

        SinkToSource(fun<void(Sink &)> writer, fun<void()> eof)
            : writer(writer)
            , eof(eof)
        {
        }

        std::string_view cur;

        size_t read(char * data, size_t len) override
        {
            bool hasCoro = coro.has_value();
            if (!hasCoro) {
                coro = coro_t::pull_type(
                    boost::coroutines2::protected_fixedsize_stack(defaultCoroutineStackSize),
                    [&](coro_t::push_type & yield) {
                        /* Feed the consumer in chunks, instead of on each write
                           to avoid excessive context switching. parseDump does
                           lots of small writes to the sink, which we should
                           accumulate. */
                        struct CoroBufferedSink : BufferedSink
                        {
                            coro_t::push_type & yield;

                            void writeUnbuffered(std::string_view data) override
                            {
                                yield(data);
                            }

                            CoroBufferedSink(coro_t::push_type & yield)
                                : yield(yield)
                            {
                            }
                        };

                        CoroBufferedSink sink(yield);
                        writer(sink);
                        sink.flush();
                    });
            }

            if (cur.empty()) {
                if (hasCoro && *coro) {
                    (*coro)();
                }
                if (*coro) {
                    cur = coro->get();
                } else {
                    coro.reset();
                    eof();
                    unreachable();
                }
            }

            size_t n = cur.copy(data, len);
            cur.remove_prefix(n);

            /* This is necessary to ensure that the coroutine gets resumed
               after the consumer has finished reading the Source. Otherwise the
               coroutine is always abandoned (i.e. it is always destroyed when
               suspended). */
            if (cur.empty() && coro && *coro) {
                (*coro)();
                if (*coro)
                    cur = coro->get();
            }

            return n;
        }
    };

    return std::make_unique<SinkToSource>(writer, eof);
}

void writePadding(size_t len, Sink & sink)
{
    if (len % 8) {
        char zero[8];
        memset(zero, 0, sizeof(zero));
        sink({zero, 8 - (len % 8)});
    }
}

void writeString(std::string_view data, Sink & sink)
{
    sink << data.size();
    sink(data);
    writePadding(data.size(), sink);
}

Sink & operator<<(Sink & sink, std::string_view s)
{
    writeString(s, sink);
    return sink;
}

template<class T>
void writeStrings(const T & ss, Sink & sink)
{
    sink << ss.size();
    for (auto & i : ss)
        sink << i;
}

Sink & operator<<(Sink & sink, const Strings & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const StringSet & s)
{
    writeStrings(s, sink);
    return sink;
}

Sink & operator<<(Sink & sink, const Error & ex)
{
    auto & info = ex.info();
    sink << "Error" << info.level << "Error" // removed
         << info.msg.str() << 0              // FIXME: info.errPos
         << info.traces.size();
    for (auto & trace : info.traces) {
        sink << 0; // FIXME: trace.pos
        sink << trace.hint.str();
    }
    return sink;
}

void readPadding(size_t len, Source & source)
{
    if (len % 8) {
        uint64_t zero = 0;
        size_t n = 8 - (len % 8);
        source(reinterpret_cast<char *>(&zero), n);
        if (zero)
            throw SerialisationError("non-zero padding");
    }
}

size_t readString(char * buf, size_t max, Source & source)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    source(buf, len);
    readPadding(len, source);
    return len;
}

std::string readString(Source & source, size_t max)
{
    auto len = readNum<size_t>(source);
    if (len > max)
        throw SerialisationError("string is too long");
    std::string res(len, 0);
    source(res.data(), len);
    readPadding(len, source);
    return res;
}

Source & operator>>(Source & in, std::string & s)
{
    s = readString(in);
    return in;
}

template<class T>
T readStrings(Source & source)
{
    auto count = readNum<size_t>(source);
    T ss;
    while (count--)
        ss.insert(ss.end(), readString(source));
    return ss;
}

template Strings readStrings(Source & source);
template StringSet readStrings(Source & source);

void StringSink::operator()(std::string_view data)
{
    s.append(data);
}

size_t ChainSource::read(char * data, size_t len)
{
    if (useSecond) {
        return source2.read(data, len);
    } else {
        try {
            return source1.read(data, len);
        } catch (EndOfFile &) {
            useSecond = true;
            return this->read(data, len);
        }
    }
}

} // namespace nix
