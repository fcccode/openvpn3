// OpenVPN client ("OpenVPNClient" class) intended for wrapping as a Java class using swig

#include <iostream>

#include "ovpncli.hpp" // objects that we bridge with java

// debug settings

#define OPENVPN_DEBUG
//#define OPENVPN_DEBUG_CLIPROTO
//#define OPENVPN_FORCE_TUN_NULL
//#define OPENVPN_DEBUG_PROTO
#define OPENVPN_DEBUG_TUN     2
#define OPENVPN_DEBUG_UDPLINK 2
#define OPENVPN_DEBUG_TCPLINK 2
//#define OPENVPN_DEBUG_COMPRESS
//#define OPENVPN_DEBUG_PACKET_ID
//#define OPENVPN_PACKET_LOG "pkt.log"

// log thread settings
#define OPENVPN_LOG_CLASS openvpn::ClientAPI::OpenVPNClient
#define OPENVPN_LOG_INFO  openvpn::ClientAPI::LogInfo

#include <openvpn/log/logthread.hpp>    // should be first included file from openvpn

#include <openvpn/init/initprocess.hpp>
#include <openvpn/common/types.hpp>
#include <openvpn/common/scoped_ptr.hpp>
#include <openvpn/client/cliconnect.hpp>

namespace openvpn {
  namespace ClientAPI {

    class MySessionStats : public SessionStats
    {
    public:
      typedef boost::intrusive_ptr<MySessionStats> Ptr;

      MySessionStats(OpenVPNClient* parent_arg)
	: parent(parent_arg)
      {
	std::memset(errors, 0, sizeof(errors));
      }

      static size_t combined_n()
      {
	return N_STATS + Error::N_ERRORS;
      }

      static std::string combined_name(const size_t index)
      {
	if (index < N_STATS + Error::N_ERRORS)
	  {
	    if (index < N_STATS)
	      return stat_name(index);
	    else
	      return Error::name(index - N_STATS);
	  }
	else
	  return "";
      }

      count_t combined_value(const size_t index) const
      {
	if (index < N_STATS + Error::N_ERRORS)
	  {
	    if (index < N_STATS)
	      return get_stat(index);
	    else
	      return errors[index - N_STATS];
	  }
	else
	  return 0;
      }

    private:
      virtual void error(const size_t err, const std::string* text=NULL)
      {
	if (err < Error::N_ERRORS)
	  ++errors[err];
      }

      OpenVPNClient* parent;
      count_t errors[Error::N_ERRORS];
    };

    class MyClientEvents : public ClientEvent::Queue
    {
    public:
      typedef boost::intrusive_ptr<MyClientEvents> Ptr;

      MyClientEvents(OpenVPNClient* parent_arg) : parent(parent_arg) {}

      virtual void add_event(const ClientEvent::Base::Ptr& event)
      {
	Event ev;
	ev.error = false;
	ev.name = event->name();
	ev.info = event->render();
	parent->event(ev);
      }

    private:
      OpenVPNClient* parent;
    };

    namespace Private {
      struct ClientState
      {
	OptionList options;
	RequestCreds req_creds;
	MySessionStats::Ptr stats;
	MyClientEvents::Ptr events;
	ClientConnect::Ptr session;
      };
    };

    inline OpenVPNClient::OpenVPNClient()
    {
      InitProcess::init();
      state = new Private::ClientState();
    }

    inline Status OpenVPNClient::parse_config(const Config& config)
    {
      Status ret;
      try {
	// parse config
	state->options.parse_from_config(config.content);
	state->options.update_map();

	// fill out RequestCreds struct
	{
	  const Option *o = state->options.get_ptr("auth-user-pass");
	  state->req_creds.autologin = !o;
	}
	{
	  const Option *o = state->options.get_ptr("static-challenge");
	  if (o)
	    {
	      state->req_creds.staticChallenge = o->get(1);
	      if (o->get(2) == "1")
		state->req_creds.staticChallengeEcho = true;
	    }
	}
      }
      catch (std::exception& e)
	{
	  ret.error = true;
	  ret.message = e.what();
	}
      return ret;
    }

    inline RequestCreds OpenVPNClient::needed_creds() const
    {
      return state->req_creds;
    }

    inline Status OpenVPNClient::connect(const ProvideCreds& creds)
    {
      boost::asio::detail::signal_blocker signal_blocker; // signals should be handled by parent thread
      Log::Context log_context(this);
      Status ret;
      bool in_run = false;
      ScopedPtr<boost::asio::io_service> io_service;

      try {
	// client stats
	state->stats.reset(new MySessionStats(this));

	// client events
	state->events.reset(new MyClientEvents(this));

	// load options
	ClientOptions::Ptr client_options = new ClientOptions(state->options, state->stats, state->events);

	// get creds if needed
	if (client_options->need_creds())
	  client_options->submit_creds(creds.username, creds.password);

	// initialize the Asio io_service object
	io_service.reset(new boost::asio::io_service(1)); // concurrency hint=1

	// instantiate top-level client session
	state->session.reset(new ClientConnect(*io_service, client_options));

	// start VPN
	state->session->start(); // queue parallel async reads

	// run i/o reactor
	in_run = true;	
	io_service->run();
      }
      catch (std::exception& e)
	{
	  if (in_run)
	    {
	      state->session->stop(); // On exception, stop client...
	      io_service->poll();     //   and execute completion handlers.
	    }
	  ret.error = true;
	  ret.message = e.what();
	}
      state->session.reset();
      return ret;
    }

    int OpenVPNClient::stats_n()
    {
      return MySessionStats::combined_n();
    }

    std::string OpenVPNClient::stats_name(int index)
    {
      return MySessionStats::combined_name(index);
    }

    long long OpenVPNClient::stats_value(int index) const
    {
      MySessionStats::Ptr stats = state->stats;
      if (stats)
	return stats->combined_value(index);
      else
	return 0;
    }

    inline void OpenVPNClient::stop()
    {
      ClientConnect::Ptr session = state->session;
      if (session)
	session->thread_safe_stop();
    }

    inline OpenVPNClient::~OpenVPNClient()
    {
      delete state;
    }
  }
}