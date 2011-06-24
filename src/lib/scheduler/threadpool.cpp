#include <string>
#include <map>
#include <set>
#include <utility>
#include <deque>

#include <boost/make_shared.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/unordered_map.hpp>

#include <ecto/plasm.hpp>
#include <ecto/tendril.hpp>
#include <ecto/module.hpp>
#include <ecto/log.hpp>
#include <ecto/strand.hpp>

#include <ecto/graph_types.hpp>
#include <ecto/plasm.hpp>
#include <ecto/scheduler/invoke.hpp>
#include <ecto/scheduler/threadpool.hpp>

#include <boost/spirit/home/phoenix/core.hpp>
#include <boost/spirit/home/phoenix/operator.hpp>
#include <boost/exception/all.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace ecto {

  using namespace ecto::graph;
  using boost::bind;
  using boost::ref;
  using boost::shared_ptr;
  using boost::thread;
  using boost::exception;
  using boost::exception_ptr;

  namespace scheduler {
    namespace asio = boost::asio;

    namespace pt = boost::posix_time;

    struct threadpool::impl 
    {
      typedef boost::function<bool(unsigned)> respawn_cb_t;

      struct propagator
      {
        asio::io_service &from, &to;
        asio::io_service::work work;

        propagator(asio::io_service& from_, asio::io_service& to_) 
          : from(from_), to(to_), work(to) { }

        void operator()() {
          from.run();
        }

        template <typename Handler>
        void post(Handler h)
        {
          to.post(h);
        }
      };

      struct thrower
      {
        exception_ptr eptr;
        thrower(exception_ptr eptr_) : eptr(eptr_) { }

        void operator()() const
        {
          rethrow_exception(eptr);
        }
      };


      struct runandjoin
      {
        typedef shared_ptr<runandjoin> ptr;

        thread runner;

        runandjoin() 
        { 
          runner.join();
        }

        ~runandjoin() {
          runner.join();
        }

        void joinit() 
        {
          runner.join();
        }

        template <typename Work>
        void impl(Work w)
        {
          //          std::cout << __PRETTY_FUNCTION__ << "\n";
          try {
            w();
          } catch (const boost::exception& e) {
            w.post(thrower(boost::current_exception()));
          } catch (const std::exception& e) {
            w.post(thrower(boost::current_exception()));
          }
          w.post(bind(&runandjoin::joinit, this));
        }

        template <typename Work>
        void run(Work w)
        {
          thread* newthread = new thread(bind(&runandjoin::impl<Work>, this, w));
          newthread->swap(runner);
        }
      };

      struct invoker : boost::noncopyable
      {
        typedef boost::shared_ptr<invoker> ptr;

        threadpool::impl& context;

        // boost::asio::deadline_timer dt;
        graph_t& g;
        graph_t::vertex_descriptor vd;
        unsigned n_calls;
        respawn_cb_t respawn;

        invoker(threadpool::impl& context_,
                graph_t& g_, 
                graph_t::vertex_descriptor vd_,
                respawn_cb_t respawn_)
          : context(context_), /*dt(context.workserv),*/ g(g_), vd(vd_), n_calls(0), respawn(respawn_)
        { }

        void async_wait_for_input()
        {
          ECTO_LOG_DEBUG("%s async_wait_for_input", this);
          namespace asio = boost::asio;

          if (inputs_ready()) {
            ECTO_LOG_DEBUG("%s inputs ready", this);
            module::ptr m = g[vd];
            if (m->strand_)
              {
                const ecto::strand& skey = *(m->strand_);
                boost::shared_ptr<asio::io_service::strand>& strand_p = context.strands[skey];
                if (!strand_p) {
                  strand_p.reset(new boost::asio::io_service::strand(context.workserv));
                }
                strand_p->post(bind(&invoker::invoke, this));
              }
            else
              {
                context.workserv.post(bind(&invoker::invoke, this));
              }
          } else {
            //            ECTO_LOG_DEBUG("%s wait", this);
            //            dt.expires_from_now(boost::posix_time::milliseconds(0));
            //            dt.wait();
            context.workserv.post(bind(&invoker::async_wait_for_input, this));
          }
        }

        void invoke()
        {
          ECTO_LOG_DEBUG("%s invoke", this);
          try {
            int j = ecto::scheduler::invoke_process(g, vd);
            if (j != ecto::OK)
              abort();
          } catch (const std::exception& e) {
            context.mainserv.post(thrower(boost::current_exception()));
            return;
          } catch (const boost::exception& e) {
            context.mainserv.post(thrower(boost::current_exception()));
            return;
          }
          ++n_calls;
          if (respawn(n_calls)) {
            context.workserv.post(bind(&invoker::async_wait_for_input, this));
          }
          else
            ECTO_LOG_DEBUG("n_calls (%u) reached, no respawn", n_calls);
        }
        
        bool inputs_ready()
        {
          graph_t::in_edge_iterator in_beg, in_end;
          for (tie(in_beg, in_end) = in_edges(vd, g);
               in_beg != in_end; ++in_beg)
            {
              graph::edge::ptr e = g[*in_beg];
              if (e->size() == 0)
                return false;
            }

          graph_t::out_edge_iterator out_beg, out_end;
          for (tie(out_beg, out_end) = out_edges(vd, g);
               out_beg != out_end; ++out_beg)
            {
              graph::edge::ptr e = g[*out_beg];
              if (e->size() > 0)
                return false;
            }

          return true;
        }

        ~invoker() { ECTO_LOG_DEBUG("%s ~invoker", this); }
      }; // struct invoker

      void reset_times(graph_t& graph)
      {
        graph_t::vertex_iterator begin, end;
        for (tie(begin, end) = vertices(graph);
             begin != end;
             ++begin)
          {
            module::ptr m = graph[*begin];
            m->stats.ncalls = 0;
            m->stats.total_ticks = 0;
          }
      }

      int execute(unsigned nthreads, impl::respawn_cb_t respawn, graph_t& graph)
      {
        namespace asio = boost::asio;

        workserv.reset();
        mainserv.reset();

        starttime = pt::microsec_clock::universal_time();
        int64_t start_ticks = profile::read_tsc();
        reset_times(graph);

        graph_t::vertex_iterator begin, end;
        for (tie(begin, end) = vertices(graph);
             begin != end;
             ++begin)
          {
            impl::invoker::ptr ip(new impl::invoker(*this, graph, *begin, respawn));
            invokers[*begin] = ip;
            workserv.post(boost::bind(&impl::invoker::async_wait_for_input, ip));
          }

        std::set<runandjoin::ptr> runners;
        for (unsigned j=0; j<nthreads; ++j)
          {
            ECTO_LOG_DEBUG("%s Start thread %u", this % j);
            runandjoin::ptr rj(new runandjoin);
            rj->run(propagator(workserv, mainserv));
            runners.insert(rj);
          }

        try {
          mainserv.run();
        } catch (const exception& e) {
          workserv.stop();
          // rethrow:  python interpreter will catch.
          throw;
        }

        pt::time_duration elapsed = pt::microsec_clock::universal_time() - starttime;
        int64_t elapsed_ticks = profile::read_tsc() - start_ticks;

        double total_percentage = 0.0;

        std::cout << "****************************************\n";
        for (tie(begin, end) = vertices(graph); begin != end; ++begin)
          {
            module::ptr m = graph[*begin];
            double this_percentage = 100.0 * ((double)m->stats.total_ticks / elapsed_ticks);
            total_percentage += this_percentage;
        
            std::cout << str(boost::format(">>> %25s calls: %u  cpu ticks: %12lu (%lf%%)")
                             % m->name()
                             % m->stats.ncalls 
                             % m->stats.total_ticks 
                             % this_percentage)
                      << "\n";
          }
              
        std::cout << "**********************************************"
                  << "\nthreads:          " << nthreads
                  << "\nelapsed time:     " << elapsed 
                  << "\ncpu ticks:        " << elapsed_ticks 
          ;

        std::cout << str(boost::format("\npercentage total: %f%%\nper-thread:       %f%%\n")
                         % total_percentage
                         % (total_percentage / nthreads))
                  << "\n";
        return 0;
      }

      ~impl() 
      {
        // be sure your invokers disappear before you do (you're
        // holding the main service)
        invokers_t().swap(invokers);
      }

      typedef std::map<graph_t::vertex_descriptor, invoker::ptr> invokers_t;
      invokers_t invokers;
      boost::asio::io_service mainserv, workserv;
      boost::unordered_map<ecto::strand, 
                           boost::shared_ptr<boost::asio::io_service::strand>,
                           ecto::strand_hash> strands;

      pt::ptime starttime;
      

    };

    threadpool::threadpool(plasm& p)
      : graph(p.graph()), impl_(new impl)
    { }

    namespace phx = boost::phoenix;

    int threadpool::execute(unsigned nthreads)
    {
      return impl_->execute(nthreads, phx::val(true), graph);
    }

    int threadpool::execute(unsigned nthreads, unsigned ncalls)
    {
      return impl_->execute(nthreads, boost::phoenix::arg_names::arg1 < ncalls, graph);
    }
  }
}
