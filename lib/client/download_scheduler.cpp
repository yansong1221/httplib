#include "download_scheduler_impl.h"

namespace httplib::client
{

    download_scheduler::download_scheduler(net::any_io_executor ex,
                                           std::shared_ptr<http_client_pool> pool,
                                           scheduler_config cfg)
        : impl_(std::make_shared<impl>(std::move(ex), std::move(pool), std::move(cfg)))
    {
    }

    download_scheduler::~download_scheduler()
    {
        if (impl_)
        {
            impl_->request_stop();
        }
    }

    download_scheduler::task_id
    download_scheduler::add(std::string_view url,
                            fs::path const& save_path,
                            task_options opts)
    {
        return impl_->add(url, save_path, std::move(opts));
    }

    void
    download_scheduler::cancel(task_id id)
    {
        impl_->cancel(id);
    }

    void
    download_scheduler::cancel_all()
    {
        impl_->cancel_all();
    }

    void
    download_scheduler::pause(task_id id)
    {
        impl_->pause(id);
    }

    void
    download_scheduler::resume(task_id id)
    {
        impl_->resume(id);
    }

    download_scheduler::task_status
    download_scheduler::get_status(task_id id) const
    {
        return impl_->get_status(id);
    }

    std::vector<download_scheduler::task_status>
    download_scheduler::get_all_status() const
    {
        return impl_->get_all_status();
    }

    std::size_t
    download_scheduler::active_count() const
    {
        return impl_->active_count();
    }

    std::size_t
    download_scheduler::pending_count() const
    {
        return impl_->pending_count();
    }

    std::size_t
    download_scheduler::total_count() const
    {
        return impl_->total_count();
    }

    void
    download_scheduler::set_progress_callback(progress_callback cb)
    {
        impl_->set_progress_callback(std::move(cb));
    }

    void
    download_scheduler::set_state_callback(state_callback cb)
    {
        impl_->set_state_callback(std::move(cb));
    }

    void
    download_scheduler::set_scheduler_config(scheduler_config const& cfg)
    {
        impl_->set_scheduler_config(cfg);
    }

    download_scheduler::scheduler_config
    download_scheduler::get_scheduler_config() const
    {
        return impl_->get_scheduler_config();
    }

    net::awaitable<void>
    download_scheduler::async_run()
    {
        co_await impl_->async_run();
    }

    net::awaitable<download_scheduler::task_status>
    download_scheduler::async_wait_any()
    {
        co_return co_await impl_->async_wait_any();
    }

    net::awaitable<download_scheduler::task_status>
    download_scheduler::async_wait_one(task_id id)
    {
        co_return co_await impl_->async_wait_one(id);
    }

    net::awaitable<void>
    download_scheduler::async_shutdown()
    {
        co_await impl_->async_shutdown();
    }

} // namespace httplib::client