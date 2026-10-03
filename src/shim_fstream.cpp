/*
 * GNU/Linux-layout file streams for the vendor code.
 *
 * std::basic_filebuf embeds a __c_lock (pthread_mutex_t: 48 bytes on glibc
 * aarch64, 64 on Darwin) and three mbstate_t (8 bytes vs 128), so Darwin's
 * libstdc++ file streams have a different size and different virtual-base
 * offsets than the ones the vendor code was compiled against (it inlines their
 * constructors and destructors). These classes reproduce the Linux layout and
 * are bound to the std:: fstream symbols the vendor code imports.
 *
 * Built with Homebrew g++ against its libstdc++ so the class hierarchies, and
 * therefore vtable/VTT shapes, match GCC's std:: versions exactly.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream> /* std::__basic_file<char> */
#include <istream>
#include <new>
#include <ostream>
#include <string>

extern "C" {
#include "shim.h"
}

namespace lx {

class filebuf : public std::streambuf {
public:
    filebuf();
    ~filebuf() override;
    filebuf *open(const char *name, std::ios_base::openmode mode);
    filebuf *close();
    bool is_open() const { return file_.is_open(); }

protected:
    std::streambuf *setbuf(char *s, std::streamsize n) override;
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override;
    pos_type seekpos(pos_type pos, std::ios_base::openmode) override;
    int sync() override;
    std::streamsize showmanyc() override;
    int_type underflow() override;
    int_type overflow(int_type c) override;

private:
    bool flush_put_area();
    void drop_get_area();
    void allocate_buffer();
    void release_buffer();

    /* Field order and sizes mirror GNU basic_filebuf<char> on aarch64. */
    char lock_[48];                   /* __c_lock (glibc pthread_mutex_t) */
    std::__basic_file<char> file_;    /* { FILE*, bool } */
    std::ios_base::openmode mode_;
    uint64_t state_beg_, state_cur_, state_last_; /* glibc mbstate_t */
    char *buf_;
    size_t buf_size_;
    bool buf_allocated_, reading_, writing_;
    char pback_;
    char *pback_cur_save_, *pback_end_save_;
    bool pback_init_;
    const void *codecvt_;
    char *ext_buf_;
    std::streamsize ext_buf_size_;
    const char *ext_next_;
    char *ext_end_;
};

class ifstream : public std::istream {
public:
    ifstream();
    ifstream(const std::string &name, std::ios_base::openmode mode);
    ~ifstream() override;
    filebuf fb_;
};

class ofstream : public std::ostream {
public:
    ofstream();
    ofstream(const std::string &name, std::ios_base::openmode mode);
    ~ofstream() override;
    void close();
    filebuf fb_;
};

class fstream : public std::iostream {
public:
    fstream();
    ~fstream() override;
    filebuf fb_;
};

/* GNU/Linux aarch64 layout (verified against the vendor's inlined code). */
static_assert(sizeof(std::streambuf) == 64, "streambuf layout");
static_assert(sizeof(filebuf) == 248, "GNU basic_filebuf<char> is 248 bytes");
static_assert(sizeof(std::__basic_file<char>) == 16, "__basic_file layout");

/* ---- filebuf ----------------------------------------------------------- */

filebuf::filebuf()
    : std::streambuf(), lock_(), file_(nullptr), mode_(), state_beg_(), state_cur_(),
      state_last_(), buf_(nullptr), buf_size_(BUFSIZ > 8192 ? BUFSIZ : 8192),
      buf_allocated_(false), reading_(false), writing_(false), pback_(), pback_cur_save_(),
      pback_end_save_(), pback_init_(false), codecvt_(nullptr), ext_buf_(nullptr),
      ext_buf_size_(0), ext_next_(nullptr), ext_end_(nullptr) {}

filebuf::~filebuf() {
    try {
        close();
    } catch (...) {
    }
}

void filebuf::allocate_buffer() {
    if (!buf_ && buf_size_) {
        buf_ = new char[buf_size_];
        buf_allocated_ = true;
    }
}

void filebuf::release_buffer() {
    if (buf_allocated_) delete[] buf_;
    buf_ = nullptr;
    buf_allocated_ = false;
}

filebuf *filebuf::open(const char *name, std::ios_base::openmode mode) {
    if (is_open()) return nullptr;
    if (!file_.open(name, mode)) {
        SHIM_TRACE("filebuf::open(%s, 0x%x) failed\n", name, (unsigned)mode);
        return nullptr;
    }
    allocate_buffer();
    mode_ = mode;
    reading_ = writing_ = false;
    setg(buf_, buf_, buf_);
    setp(nullptr, nullptr);
    if ((mode & std::ios_base::ate) &&
        seekoff(0, std::ios_base::end, mode) == pos_type(off_type(-1))) {
        close();
        return nullptr;
    }
    SHIM_TRACE("filebuf::open(%s, 0x%x)\n", name, (unsigned)mode);
    return this;
}

filebuf *filebuf::close() {
    if (!is_open()) return nullptr;
    bool ok = !writing_ || flush_put_area();
    reading_ = writing_ = false;
    setg(nullptr, nullptr, nullptr);
    setp(nullptr, nullptr);
    release_buffer();
    if (!file_.close()) ok = false;
    return ok ? this : nullptr;
}

std::streambuf *filebuf::setbuf(char *s, std::streamsize n) {
    if (!is_open()) {
        release_buffer();
        if (!s && !n) {
            buf_size_ = 0; /* unbuffered: underflow/overflow use 1-byte buffer */
        } else if (s && n > 0) {
            buf_ = s;
            buf_size_ = (size_t)n;
        }
    }
    return this;
}

bool filebuf::flush_put_area() {
    std::streamsize n = pptr() - pbase();
    if (n > 0 && file_.xsputn(pbase(), n) != n) return false;
    setp(pbase(), epptr());
    return true;
}

void filebuf::drop_get_area() {
    /* Move the file position back over bytes read ahead but not consumed. */
    std::streamsize unread = egptr() - gptr();
    if (unread > 0) file_.seekoff(-unread, std::ios_base::cur);
    setg(buf_, buf_, buf_);
}

filebuf::int_type filebuf::underflow() {
    if (!(mode_ & std::ios_base::in) || !is_open()) return traits_type::eof();
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (writing_) {
        if (!flush_put_area()) return traits_type::eof();
        setp(nullptr, nullptr);
        writing_ = false;
    }
    reading_ = true;
    char *b = buf_ ? buf_ : &pback_;
    std::streamsize cap = buf_ ? (std::streamsize)buf_size_ : 1;
    std::streamsize n = file_.xsgetn(b, cap);
    if (n <= 0) {
        setg(b, b, b);
        return traits_type::eof();
    }
    setg(b, b, b + n);
    return traits_type::to_int_type(*gptr());
}

filebuf::int_type filebuf::overflow(int_type c) {
    if (!(mode_ & (std::ios_base::out | std::ios_base::app)) || !is_open())
        return traits_type::eof();
    if (reading_) {
        drop_get_area();
        setg(nullptr, nullptr, nullptr);
        reading_ = false;
    }
    if (!writing_) {
        if (buf_ && buf_size_ > 1) setp(buf_, buf_ + buf_size_ - 1); /* keep 1 for c */
        else setp(nullptr, nullptr);
        writing_ = true;
    }
    if (traits_type::eq_int_type(c, traits_type::eof()))
        return flush_put_area() ? traits_type::not_eof(c) : traits_type::eof();
    if (pbase()) {
        *pptr() = traits_type::to_char_type(c);
        pbump(1);
        return flush_put_area() ? c : traits_type::eof();
    }
    char ch = traits_type::to_char_type(c);
    return file_.xsputn(&ch, 1) == 1 ? c : traits_type::eof();
}

int filebuf::sync() {
    if (writing_ && !flush_put_area()) return -1;
    if (writing_) file_.sync();
    if (reading_) drop_get_area();
    return 0;
}

filebuf::pos_type filebuf::seekoff(off_type off, std::ios_base::seekdir dir,
                                   std::ios_base::openmode) {
    if (!is_open()) return pos_type(off_type(-1));
    if (writing_ && !flush_put_area()) return pos_type(off_type(-1));
    if (reading_) {
        if (dir == std::ios_base::cur) off -= egptr() - gptr();
        setg(buf_, buf_, buf_);
    }
    reading_ = writing_ = false;
    setp(nullptr, nullptr);
    std::streamoff r = file_.seekoff(off, dir);
    return pos_type(r);
}

filebuf::pos_type filebuf::seekpos(pos_type pos, std::ios_base::openmode mode) {
    return seekoff(off_type(pos), std::ios_base::beg, mode);
}

std::streamsize filebuf::showmanyc() {
    if (!(mode_ & std::ios_base::in) || !is_open()) return -1;
    return (egptr() - gptr()) + file_.showmanyc();
}

/* ---- streams ----------------------------------------------------------- */

ifstream::ifstream() : std::istream(nullptr), fb_() { init(&fb_); }
ifstream::ifstream(const std::string &name, std::ios_base::openmode mode) : ifstream() {
    if (!fb_.open(name.c_str(), mode | std::ios_base::in)) setstate(std::ios_base::failbit);
    else clear();
}
ifstream::~ifstream() {}

ofstream::ofstream() : std::ostream(nullptr), fb_() { init(&fb_); }
ofstream::ofstream(const std::string &name, std::ios_base::openmode mode) : ofstream() {
    if (!fb_.open(name.c_str(), mode | std::ios_base::out)) setstate(std::ios_base::failbit);
    else clear();
}
ofstream::~ofstream() {}
void ofstream::close() {
    if (!fb_.close()) setstate(std::ios_base::failbit);
}

fstream::fstream() : std::iostream(nullptr), fb_() { init(&fb_); }
fstream::~fstream() {}

static_assert(offsetof(ifstream, fb_) == 16, "ifstream: filebuf follows istream");
static_assert(offsetof(fstream, fb_) == 24, "fstream: filebuf follows iostream");

} // namespace lx

/* ---- C-ABI entry points bound to the std:: symbols ----------------------- */

namespace {
void filebuf_ctor(void *p) { new (p) lx::filebuf(); }
void filebuf_dtor(lx::filebuf *p) { p->~filebuf(); }
lx::filebuf *filebuf_open(lx::filebuf *p, const char *n, std::ios_base::openmode m) {
    return p->open(n, m);
}
lx::filebuf *filebuf_close(lx::filebuf *p) { return p->close(); }

void ifstream_ctor(void *p) { new (p) lx::ifstream(); }
void ifstream_ctor_str(void *p, const std::string &n, std::ios_base::openmode m) {
    new (p) lx::ifstream(n, m);
}
void ifstream_dtor(lx::ifstream *p) { p->~ifstream(); }
/* basic_ifstream::open(const std::string&, openmode): a default-constructed stream opened
 * afterwards (SDK 0.55.100 reads the device config file this way). Without this binding
 * the call lands in Darwin's libstdc++, which expects its own, larger, layout. */
void ifstream_open_str(lx::ifstream *p, const std::string &n, std::ios_base::openmode m) {
    if (!p->fb_.open(n.c_str(), m | std::ios_base::in)) p->setstate(std::ios_base::failbit);
    else p->clear();
}

void ofstream_ctor_str(void *p, const std::string &n, std::ios_base::openmode m) {
    new (p) lx::ofstream(n, m);
}
void ofstream_dtor(lx::ofstream *p) { p->~ofstream(); }
void ofstream_close(lx::ofstream *p) { p->close(); }
bool ofstream_is_open(lx::ofstream *p) { return p->fb_.is_open(); }

void fstream_dtor(lx::fstream *p) { p->~fstream(); }

/* std::streampos is fpos<mbstate_t>: 16 bytes on glibc (returned in x0:x1),
 * 136 bytes on Darwin (returned via x8). */
struct linux_fpos {
    int64_t off;
    uint64_t state;
};
linux_fpos istream_tellg(std::istream *is) {
    return linux_fpos{static_cast<int64_t>(std::streamoff(is->tellg())), 0};
}
} // namespace

extern "C" const char lx_vtbl_filebuf[] __asm__("__ZTVN2lx7filebufE");
extern "C" const char lx_vtbl_ifstream[] __asm__("__ZTVN2lx8ifstreamE");
extern "C" const char lx_vtbl_ofstream[] __asm__("__ZTVN2lx8ofstreamE");
extern "C" const char lx_vtbl_fstream[] __asm__("__ZTVN2lx7fstreamE");
extern "C" const char lx_vtt_ifstream[] __asm__("__ZTTN2lx8ifstreamE");
extern "C" const char lx_vtt_ofstream[] __asm__("__ZTTN2lx8ofstreamE");
extern "C" const char lx_vtt_fstream[] __asm__("__ZTTN2lx7fstreamE");

#define SYM(name, addr) {name, const_cast<void *>(reinterpret_cast<const void *>(addr))}

extern "C" const struct shim_sym shim_fstream_syms[] = {
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev", &filebuf_ctor),
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEEC2Ev", &filebuf_ctor),
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", &filebuf_dtor),
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEED2Ev", &filebuf_dtor),
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode", &filebuf_open),
    SYM("_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv", &filebuf_close),
    SYM("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1Ev", &ifstream_ctor),
    SYM("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
        &ifstream_ctor_str),
    SYM("_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev", &ifstream_dtor),
    SYM("_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
        &ifstream_open_str),
    SYM("_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode",
        &ofstream_ctor_str),
    SYM("_ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev", &ofstream_dtor),
    SYM("_ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv", &ofstream_close),
    SYM("_ZNSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv", &ofstream_is_open),
    SYM("_ZNSt13basic_fstreamIcSt11char_traitsIcEED1Ev", &fstream_dtor),
    SYM("_ZNSi5tellgEv", &istream_tellg),
    SYM("_ZTVSt13basic_filebufIcSt11char_traitsIcEE", lx_vtbl_filebuf),
    SYM("_ZTVSt14basic_ifstreamIcSt11char_traitsIcEE", lx_vtbl_ifstream),
    SYM("_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE", lx_vtbl_ofstream),
    SYM("_ZTVSt13basic_fstreamIcSt11char_traitsIcEE", lx_vtbl_fstream),
    SYM("_ZTTSt14basic_ifstreamIcSt11char_traitsIcEE", lx_vtt_ifstream),
    SYM("_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE", lx_vtt_ofstream),
    SYM("_ZTTSt13basic_fstreamIcSt11char_traitsIcEE", lx_vtt_fstream),
    {nullptr, nullptr},
};
