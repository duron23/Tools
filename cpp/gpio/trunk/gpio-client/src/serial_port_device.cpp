#include "../include/gpio/serial_port_device.h"

#include <iostream>
#include <string>
#include <queue>
#include <cmath>
#include <map>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/array.hpp>
#include <boost/chrono.hpp>
#include <boost/thread/future.hpp>
#include <boost/ptr_container/ptr_unordered_map.hpp>

using namespace boost;
using namespace boost::asio;

namespace gpio {

struct gpi_port_handler
{
    virtual ~gpi_port_handler() { }

    virtual void handle(bool port_state) = 0;
};

class gpi_port_pulse_handler : public gpi_port_handler
{
    voltage silent_state_;
    chrono::milliseconds duration_;
    bool last_port_state_;
    chrono::high_resolution_clock::time_point begin_pulse_;
    gpi_trigger_handler handler_;
public:
    gpi_port_pulse_handler(
            voltage silent_state,
            int duration_milliseconds,
            const gpi_trigger_handler& handler)
        : silent_state_(silent_state)
        , duration_(duration_milliseconds)
        , last_port_state_(false)
        , handler_(handler)
    {
    }

    virtual void handle(bool port_state)
    {
        if (silent_state_ == LOW)
        {
            if (!last_port_state_ && port_state) // RISING
            {
                begin_pulse_ = chrono::high_resolution_clock::now();
            }
            else if (last_port_state_ && !port_state) // FALLING
            {
                chrono::high_resolution_clock::time_point now =
                        chrono::high_resolution_clock::now();

                if (now - begin_pulse_ >= duration_)
                    handler_();
            }
        }
        else if (silent_state_ == HIGH)
        {
            if (last_port_state_ && !port_state) // FALLING
            {
                begin_pulse_ = chrono::high_resolution_clock::now();
            }
            else if (!last_port_state_ && port_state) // RISING
            {
                chrono::high_resolution_clock::time_point now =
                        chrono::high_resolution_clock::now();

                if (now - begin_pulse_ >= duration_)
                    handler_();
            }
        }

        last_port_state_ = port_state;
    }
};

class gpi_port_tally_handler : public gpi_port_handler
{
    voltage off_state_;
    gpi_switch_handler handler_;
public:
    gpi_port_tally_handler(
            voltage off_state, const gpi_switch_handler& handler)
        : off_state_(off_state)
        , handler_(handler)
    {
    }

    virtual void handle(bool port_state)
    {
        if (off_state_ == LOW)
            handler_(port_state);
        else
            handler_(!port_state);
    }
};

typedef array<char, 4> payload_data;

struct write_package
{
    payload_data payload;
    function<void ()> completion_handler;

    write_package()
    {
        payload[2] = '\r';
        payload[3] = '\n';
    }
};

template<class Func>
class delayed_task
{
    Func task_;
    bool throw_after_;
public:
    delayed_task(const Func& task, bool throw_after = false)
        : task_(task)
        , throw_after_(throw_after)
    {
    }

    void operator()(const system::error_code& e)
    {
        if (throw_after_)
            task_();

        if (e)
        {
            system::system_error ex(e);
            std::cerr << ex.what() << std::endl;
            throw ex;
        }

        if (!throw_after_)
            task_();
    }
};

class writer
{
    io_service& service;
    std::queue<write_package>& outgoing;
    function<void ()> write_pending;
    function<void ()> check_failed;
    deadline_timer timer;
public:
    writer(
            io_service& service,
            std::queue<write_package>& outgoing,
            const function<void ()>& write_pending,
            const function<void ()>& check_failed)
        : service(service)
        , outgoing(outgoing)
        , write_pending(write_pending)
        , check_failed(check_failed)
        , timer(service)
    {
    }

    void write(const write_package& package)
    {
        check_failed();
        service.post(bind(&writer::do_write, this, package));
    }

    template<class Func>
    void post(const Func& task)
    {
        check_failed();
        service.post(task);
    }

    template<class Func>
    void post_delayed(long millis_delayed, const Func& task)
    {
        check_failed();
        timer.expires_from_now(
                posix_time::milliseconds(millis_delayed));
        timer.async_wait(delayed_task<Func>(task, true));
    }
private:
    void do_write(const write_package& package)
    {
        outgoing.push(package);
        write_pending();
    }
};

class gpo_switch_impl : public gpo_switch
{
    int port_;
    voltage off_state_;
    bool current_state_;
    weak_ptr<writer> writer_;
public:
    gpo_switch_impl(int port, voltage off_state, weak_ptr<writer> writer)
        : port_(port)
        , off_state_(off_state)
        , writer_(writer)
    {
        set(false);
    }

    virtual void set(bool state)
    {
        shared_ptr<writer> strong = writer_.lock();

        if (strong)
        {
            write_package package;
            package.payload[0] = '0' + port_;
            package.payload[1] = off_state_ == LOW
                    ? (state ? '1' : '0')
                    : (state ? '0' : '1');

            strong->write(package);
        }
        else
        {
            throw gpo_handle_expired("GPO switch is expired");
        }
    }

    virtual bool get() const
    {
        return current_state_;
    }

    virtual void toggle()
    {
        set(!current_state_);
    }
};

class gpo_trigger_impl : public gpo_trigger
{
    int port_;
    voltage silent_state_;
    int duration_milliseconds_;
    weak_ptr<writer> writer_;
    int pending;
    bool triggering;
public:
    gpo_trigger_impl(
            int port,
            voltage silent_state,
            int duration_milliseconds,
            const shared_ptr<writer>& writer)
        : port_(port)
        , silent_state_(silent_state)
        , duration_milliseconds_(duration_milliseconds)
        , writer_(writer)
        , pending(0)
        , triggering(false)
    {
    }

    virtual void fire()
    {
        shared_ptr<writer> strong = writer_.lock();

        if (strong)
        {
            strong->post(bind(&gpo_trigger_impl::increment_and_process,
                    this, strong));
        }
        else
        {
            throw gpo_handle_expired("GPO trigger is expired");
        }
    }
private:
    void increment_and_process(const shared_ptr<writer>& strong)
    {
        ++pending;
        process_pending(strong);
    }

    void process_pending(const shared_ptr<writer>& writer)
    {
        if (pending == 0 || triggering)
            return;

        write_package package;
        package.payload[0] = port_ + '0';
        package.payload[1] = silent_state_ == LOW ? '1' : '0';
        package.completion_handler = bind(
                &gpo_trigger_impl::schedule_delayed_pulse_off, this, writer);

        writer->write(package);
        triggering = true;
    }

    void schedule_delayed_pulse_off(const shared_ptr<writer>& writer)
    {
        writer->post_delayed(duration_milliseconds_,
                bind(&gpo_trigger_impl::write_pulse_off, this, writer));
    }

    void write_pulse_off(const shared_ptr<writer>& writer)
    {
        write_package package;
        package.payload[0] = port_ + '0';
        package.payload[1] = silent_state_ == LOW ? '0' : '1';
        package.completion_handler =
                bind(&gpo_trigger_impl::written_pulse_off, this);

        writer->write(package);
    }

    void written_pulse_off()
    {
        triggering = false;

        if (--pending == 0)
            return;

        shared_ptr<gpio::writer> strong = writer_.lock();

        if (strong)
        {
             process_pending(strong);
        }
    }
};

struct serial_port_device::impl
{
    io_service service;
    asio::serial_port serial_port;
    deadline_timer timer;
    array<char, 4> read_buffer;
    ptr_unordered_map<int, gpi_port_handler> gpi_handlers;
    std::map<int, shared_ptr<writer> > writers;
    std::queue<write_package> outgoing;
    bool writing;
    int num_gpi;
    int num_gpo;
    int min_supported_duration;
    basic_streambuf<> description_buffer;
    std::string description;
    promise<void> greeting_promise;
    thread io_thread;
    boost::exception_ptr io_failure;

    impl(const std::string& serial_port_name,
         int baud,
         bool spontaneous_greeting)
        : serial_port(service, serial_port_name)
        , timer(service)
        , writing(false)
    {
        serial_port.set_option(serial_port_base::baud_rate(baud));
        serial_port.set_option(serial_port_base::character_size(8));
        serial_port.set_option(serial_port_base::flow_control(
                serial_port_base::flow_control::none));
        serial_port.set_option(serial_port_base::parity(
                serial_port_base::parity::none));
        serial_port.set_option(serial_port_base::stop_bits(
                serial_port_base::stop_bits::one));

        //serial_port.open(serial_port_name);

        io_thread = thread(bind(&impl::run, this, spontaneous_greeting));

        unique_future<void> greeting_received = greeting_promise.get_future();

        if (!greeting_received.timed_wait(posix_time::milliseconds(5000)))
            throw std::runtime_error(
                    "Device did not answer with greeting in time");

        description += " on " + serial_port_name;
        min_supported_duration = std::ceil(1000.0 / (baud / 16.0));

        if (min_supported_duration == 0)
            min_supported_duration = 1;
    }

    void run(bool spontaneous_greeting)
    {
        if (!spontaneous_greeting)
            delay(500, bind(&impl::read_num_gpio, this));
        else
            delay(500, bind(&impl::say_hi, this));

        try
        {
            service.run();
        }
        catch (...)
        {
            io_failure = current_exception();
            service.stop();
        }
    }

    void throw_if_failed()
    {
        if (!(io_failure == 0))
            rethrow_exception(io_failure);
    }

    template<class Func>
    void delay(long delay_millis, const Func& command)
    {
        timer.expires_from_now(posix_time::milliseconds(delay_millis));
        timer.async_wait(delayed_task<Func>(command));
    }

    void say_hi()
    {
        write_package package;
        package.payload[0] = 'h';
        package.payload[1] = 'i';
        package.completion_handler = bind(&impl::read_num_gpio, this);

        outgoing.push(package);
        write_pending();
        std::cout << "say_hi" << std::endl;
    }

    void read_num_gpio()
    {
        std::cout << "begin read_num_gpio" << std::endl;
        async_read(
                serial_port,
                buffer(read_buffer),
                transfer_exactly(2),
                bind(&impl::on_read_num_gpio, this, _1, _2));
    }

    void on_read_num_gpio(
        const system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
            throw system::system_error(e);

        if (bytes_transferred == 2)
        {
            std::cout << "on_read_num_gpio" << std::endl;
            num_gpi = read_buffer[0] - '0';
            num_gpo = read_buffer[1] - '0';

            async_read_until(
                    serial_port,
                    description_buffer,
                    std::string("\r\n", 2),
                    bind(&impl::on_read_description, this, _1, _2));
        }
    }

    void on_read_description(
            const system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
            throw system::system_error(e);

        if (bytes_transferred == 0)
            return;

        std::cout << "on_read_description" << std::endl;

        std::stringstream descr;
        std::istream buff_stream(&description_buffer);
        descr << buff_stream.rdbuf();
        description = descr.str();
        description.resize(description.size() - 2); // Remove \r\n

        greeting_promise.set_value();
        read_next();
    }

    void read_next()
    {
        async_read(
                serial_port,
                buffer(read_buffer),
                transfer_exactly(read_buffer.size()),
                bind(&impl::on_read, this, _1, _2));
    }

    void on_read(const system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
            throw system::system_error(e);

        if (bytes_transferred == read_buffer.size())
        {
            char port = read_buffer[0];
            int gpi_port = port - '0';

            verify_gpi(gpi_port);

            ptr_unordered_map<int, gpi_port_handler>::iterator iter =
                    gpi_handlers.find(gpi_port);

            if (iter != gpi_handlers.end())
                iter->second->handle(read_buffer[1] == '1');
            else
                std::cout << "Unsubsribed GPI event on port " << gpi_port
                          << std::endl;
        }

        read_next();
    }

    void write_pending()
    {
        if (outgoing.empty() || writing)
            return;

        write_package& package = outgoing.back();

        writing = true;
        async_write(
                serial_port,
                buffer(package.payload),
                transfer_exactly(package.payload.size()),
                bind(&impl::on_wrote_pending, this, _1, _2));
    }

    void on_wrote_pending(
            const system::error_code& e, std::size_t bytes_transferred)
    {
        if (e)
            throw system::system_error(e);

        write_package& package = outgoing.back();

        if (bytes_transferred != package.payload.size())
            return;

        if (package.completion_handler)
        {
            package.completion_handler();
        }

        outgoing.pop();

        writing = false;
        write_pending();
    }

    shared_ptr<writer> new_writer(int gpo_port)
    {
        shared_ptr<writer> result(new writer(
                service,
                outgoing,
                bind(&impl::write_pending, this),
                bind(&impl::throw_if_failed, this)));
        service.post(bind(&impl::do_insert_writer, this, gpo_port, result));

        return result;
    }

    void do_insert_writer(int gpo_port, const shared_ptr<writer>& writer)
    {
        writers.insert(std::make_pair(gpo_port, writer));
    }

    void verify_gpi(int gpi_port)
    {
        if (gpi_port < 0 || gpi_port > num_gpi - 1)
            throw std::out_of_range("GPI port out of range");
    }

    void verify_gpo(int gpo_port)
    {
        if (gpo_port < 0 || gpo_port > num_gpo - 1)
            throw std::out_of_range("GPO port out of range");
    }

    void setup_gpi_pulse(
            int gpi_port,
            voltage silent_state,
            int duration_milliseconds,
            const gpi_trigger_handler& handler)
    {
        verify_gpi(gpi_port);
        service.post(bind(&impl::do_setup_gpi_pulse, this,
                gpi_port, silent_state, duration_milliseconds, handler));
    }

    void do_setup_gpi_pulse(
            int gpi_port,
            voltage silent_state,
            int duration_milliseconds,
            const gpi_trigger_handler& handler)
    {
        gpi_handlers.insert(
                gpi_port,
                new gpi_port_pulse_handler(
                        silent_state, duration_milliseconds, handler));
    }

    void setup_gpi_tally(
            int gpi_port,
            voltage off_state,
            const gpi_switch_handler& handler)
    {
        verify_gpi(gpi_port);
        service.post(bind(&impl::do_setup_gpi_tally, this,
                gpi_port, off_state, handler));
    }

    void do_setup_gpi_tally(
        int gpi_port,
        voltage off_state,
        const gpi_switch_handler& handler)
    {
        gpi_handlers.insert(
                gpi_port,
                new gpi_port_tally_handler(off_state, handler));
    }

    void stop_gpi(int gpi_port)
    {
        verify_gpi(gpi_port);
        service.post(bind(&impl::do_stop_gpi, this, gpi_port));
    }

    void do_stop_gpi(int gpi_port)
    {
        gpi_handlers.erase(gpi_port);
    }

    gpo_trigger::ptr setup_gpo_pulse(
            int gpo_port, voltage silent_state, int duration_milliseconds)
    {
        verify_gpo(gpo_port);

        return gpo_trigger::ptr(new gpo_trigger_impl(
                gpo_port,
                silent_state,
                duration_milliseconds,
                new_writer(gpo_port)));
    }

    gpo_switch::ptr setup_gpo_tally(int gpo_port, voltage off_state)
    {
        verify_gpo(gpo_port);

        return gpo_switch::ptr(new gpo_switch_impl(
                gpo_port, off_state, new_writer(gpo_port)));
    }

    ~impl()
    {
        service.stop();
        io_thread.join();
    }
};

serial_port_device::serial_port_device(
        const std::string& serial_port,
        int baud_rate,
        bool spontaneous_greeting)
    : impl_(new impl(serial_port, baud_rate, spontaneous_greeting))
{
}

gpio_device::ptr serial_port_device::create(
        const std::string& serial_port,
        int baud_rate,
        bool spontaneous_greeting)
{
    return gpio_device::ptr(
        new serial_port_device(serial_port, baud_rate, spontaneous_greeting));
}

serial_port_device::~serial_port_device()
{
}

std::string serial_port_device::get_description() const
{
    return impl_->description;
}

int serial_port_device::get_minimum_supported_duration() const
{
    return impl_->min_supported_duration;
}

int serial_port_device::get_num_gpi_ports() const
{
    return impl_->num_gpi;
}

int serial_port_device::get_num_gpo_ports() const
{
    return impl_->num_gpo;
}

void serial_port_device::setup_gpi_pulse(
        int gpi_port,
        voltage silent_state,
        int duration_milliseconds,
        const gpi_trigger_handler& handler)
{
    impl_->setup_gpi_pulse(
            gpi_port, silent_state, duration_milliseconds, handler);
}

void serial_port_device::setup_gpi_tally(
        int gpi_port,
        voltage off_state,
        const gpi_switch_handler& handler)
{
    impl_->setup_gpi_tally(gpi_port, off_state, handler);
}

void serial_port_device::stop_gpi(int gpi_port)
{
    impl_->stop_gpi(gpi_port);
}

gpo_trigger::ptr serial_port_device::setup_gpo_pulse(
        int gpo_port, voltage silent_state, int duration_milliseconds)
{
    return impl_->setup_gpo_pulse(gpo_port, silent_state, duration_milliseconds);
}

gpo_switch::ptr serial_port_device::setup_gpo_tally(
        int gpo_port, voltage off_state)
{
    return impl_->setup_gpo_tally(gpo_port, off_state);
}

}
