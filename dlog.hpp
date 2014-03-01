#include <mutex>
#include <condition_variable>
#include <utility>
#include <cstring>

namespace dlog {
    class writer {
    public:
        enum Result
        {
            SUCCESS,
            ERROR_TRY_LATER,
            ERROR_GIVE_UP
        };
        virtual ~writer() = 0;
        virtual Result write(void const* pbuffer, std::size_t count) = 0;
    };

    class file_writer : public writer {
    public:
        file_writer(char const* path);
        ~file_writer();
        Result write(void const* pbuffer, std::size_t count);
    private:
        int fd_;
    };

    class output_buffer {
    public:
        output_buffer(writer* pwriter, std::size_t max_capacity);
        ~output_buffer();
        char* reserve(std::size_t size);
        void commit(std::size_t size);
        void flush();

    private:

        writer* pwriter_;
        char* pbuffer_;
        char* pcommit_end_;
        char* pbuffer_end_;
    };

    void initialize(writer* pwriter);
    void initialize(writer* pwriter, std::size_t max_capacity);
    void cleanup();

    class initializer {
    public:
        initializer(writer* pwriter)
        {
            initialize(pwriter);
        }
        initializer(writer* pwriter, std::size_t max_capacity)
        {
            initialize(pwriter, max_capacity);
        }
        ~initializer()
        {
            cleanup();
        }
    };

    namespace detail {
        // max_align_t is not available in clang?
        std::size_t const FRAME_ALIGNMENT = 16u;
        struct input_buffer {
            char* pfirst;
            char* plast;
            char* pflush_start;
            char* pflush_end;
            char* pwritten_end;
        };
        extern input_buffer g_input_buffer; 
        //std::mutex buffer_mutex;

        template <typename... Args>
        struct typelist {
        };

        template <typename T>
        struct make_pointer_from_array
        {
            using type = T;
        };

        template <typename T, std::size_t Size>
        struct make_pointer_from_array<T (&) [Size]>
        {
            using type = T*;
        };

        template <typename T>
        T align(T p, std::size_t alignment)
        {
            std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
            v = ((v + alignment-1)/alignment)*alignment;
            return reinterpret_cast<T>(v);
        }

        template <class... Args>
        void evaluate(Args&&...)
        {
        }

        template <typename T>
        int destroy(T* p)
        {
            p->~T();
            return 0;
        }

        template <std::size_t Offset, typename Type>
        struct bound_argument {
            using type = Type;
            static std::size_t const offset = Offset;
        };

        typedef std::size_t dispatch_function_t(output_buffer*, char*);
        template <class Formatter, std::size_t FrameSize, class BoundArgs>
        struct frame;
        template <class Formatter, std::size_t FrameSize, class... BoundArgs>
        struct frame<Formatter, FrameSize, typelist<BoundArgs...>> {
            template<typename... Args>
            static void store_args(char* pbuffer, Args&&... args)
            {
                *reinterpret_cast<dispatch_function_t**>(pbuffer) = &frame::dispatch;
                // TODO we should do this recursively probably, because g++
                // seems to push arguments backwards and i'm not sure that's
                // good for the cache. See wikipedia page on varadic templates,
                // it has info on how to get deterministic evaluation order.
                evaluate(new (pbuffer + BoundArgs::offset) typename BoundArgs::type(args)...);
            }
            static std::size_t dispatch(output_buffer* poutput, char* pinput)
            {
                Formatter::format(poutput,
                    std::forward<typename BoundArgs::type>(
                        *reinterpret_cast<typename BoundArgs::type*>(pinput + BoundArgs::offset)
                    )...);
                evaluate(destroy(reinterpret_cast<typename BoundArgs::type*>(pinput + BoundArgs::offset))...);
                return FrameSize;
            }
        };

        template <class Accumulator, std::size_t Offset, class... Args>
        struct bind_args_helper;
        template <class Accumulator, std::size_t Offset>
        struct bind_args_helper<Accumulator, Offset> {
            using type = Accumulator;
            static std::size_t const frame_size = Offset;
        };
        template <class... Accumulator, std::size_t Offset, class Arg, class... RemainingArgs>
        struct bind_args_helper<typelist<Accumulator...>, Offset, Arg, RemainingArgs...> {
            using Value = typename std::remove_reference<typename make_pointer_from_array<Arg>::type>::type;
            static std::size_t const my_offset = (Offset + alignof(Value)-1)/alignof(Value)*alignof(Value);
            using next = bind_args_helper<
                typelist<Accumulator..., bound_argument<my_offset, Value>>,
                my_offset + sizeof(Value),
                RemainingArgs...>;
            using type = typename next::type;
            static std::size_t const frame_size = next::frame_size;
        };

        template <class... Args>
        class bind_args {
        private:
            // Note that we start not at offset 0 but at sizeof(dispatch_function_t*).
            // This is to make room for the pointer to frame::dispatch, which
            // takes care of picking up all the values from the buffer before
            // calling the proper output function.
            using helper = bind_args_helper<typelist<>, sizeof(dispatch_function_t*), Args...>;
        public:
            using type = typename helper::type;
            static std::size_t const frame_size = helper::frame_size;
        };
    }

    template <class Formatter>
    class logger {
    public:
        template <typename... Args>
        static void write(Args&&... args)
        {
            using namespace dlog::detail;
            //std::unique_lock<std::mutex> hold(buffer_mutex);
            using argument_binder = bind_args<Args...>;
            //using bound_args = typename argument_binder::type;    // fails in gcc 4.7.3
            using bound_args = typename bind_args<Args...>::type;
            std::size_t const frame_size = argument_binder::frame_size;
            using frame = detail::frame<Formatter, frame_size, bound_args>;

            char* pwrite_start = align(g_input_buffer.pwritten_end, FRAME_ALIGNMENT);
            //while(pwrite_start + frame_size > plast)
            //    commit_finish_condition.wait();
            //store_arg(bound_args(), 
            frame::store_args(pwrite_start, std::forward<Args>(args)...);
            g_input_buffer.pwritten_end = pwrite_start + frame_size;
            //frame::dispatch(&buf, pwrite_start);
        }
    };

    template <typename... Args>
    void line(Args... args)
    {
    }

    inline void flush()
    {
    }

    bool format(output_buffer* pbuffer, char const*& pformat, char v);
    bool format(output_buffer* pbuffer, char const*& pformat, signed char v);
    bool format(output_buffer* pbuffer, char const*& pformat, unsigned char v);
    bool format(output_buffer* pbuffer, char const*& pformat, wchar_t v);
    bool format(output_buffer* pbuffer, char const*& pformat, char16_t v);
    bool format(output_buffer* pbuffer, char const*& pformat, char32_t v);

    bool format(output_buffer* pbuffer, char const*& pformat, short v);
    bool format(output_buffer* pbuffer, char const*& pformat, unsigned short v);
    bool format(output_buffer* pbuffer, char const*& pformat, int v);
    bool format(output_buffer* pbuffer, char const*& pformat, unsigned int v);
    bool format(output_buffer* pbuffer, char const*& pformat, long v);
    bool format(output_buffer* pbuffer, char const*& pformat, unsigned long v);
    bool format(output_buffer* pbuffer, char const*& pformat, long long v);
    bool format(output_buffer* pbuffer, char const*& pformat, unsigned long long v);

    bool format(output_buffer* pbuffer, char const*& pformat, float v);
    bool format(output_buffer* pbuffer, char const*& pformat, double v);
    bool format(output_buffer* pbuffer, char const*& pformat, long double v);

    bool format(output_buffer* pbuffer, char const*& pformat, char const* v);
    bool format(output_buffer* pbuffer, char const*& pformat, std::string const& v);

    template <typename T>
    bool invoke_custom_format(output_buffer* pbuffer, char const*& pformat, T&& v)
    {
        return format(pbuffer, pformat, std::forward<T>(v));
    }

    class formatter {
    public:
        static void format(output_buffer* pbuffer, char const* pformat)
        {
            auto len = std::strlen(pformat);
            char* p = pbuffer->reserve(len);
            std::memcpy(p, pformat, len);
            pbuffer->commit(len);
        }

        template <typename T, typename... Args>
        static void format(output_buffer* pbuffer, char const* pformat, T&& value, Args&&... args)
        {
            while(true) {
                char const* pspecifier = std::strchr(pformat, '%');
                if(pspecifier == nullptr)
                    return format(pbuffer, pformat);

                auto len = pspecifier - pformat;
                char* p = pbuffer->reserve(len);
                std::memcpy(p, pformat, len);
                pbuffer->commit(len);

                pformat = pspecifier + 1;

                if(*pformat == '%') {
                    p = pbuffer->reserve(1u);
                    *p = '%';
                    pbuffer->commit(1u);
                    ++pformat;
                } else {
                    if(invoke_custom_format(pbuffer, pformat, std::forward<T>(value)))
                        return formatter::format(pbuffer, pformat, std::forward<Args>(args)...);
                }
            }
        }
    };
}