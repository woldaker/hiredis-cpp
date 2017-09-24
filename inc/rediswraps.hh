#ifndef REDISWRAPS_HH
#define REDISWRAPS_HH
// VERSION: THROWS

#include <cstring> // strcpy() used in format_cmd_args()

#include <array>         // used in cmd_proxy()
#include <deque>         // Holds all the response strings from Redis
#include <iostream>
#include <fstream>       // used in read_file()
#include <memory>        // typedef for std::unique_ptr<Connection>
#include <sstream>       // used in read_file()
#include <string>
#include <type_traits>   // enable_if<>...
#include <unordered_map> // Maps Lua scripts to their hash digests.
#include <utility>       // std::forward, std::pair

#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>

extern "C" {
#include <hiredis/hiredis.h>
}

#ifndef inline_
#define inline_ inline
#endif


namespace rediswraps {

namespace cmd { // {{{
// Used as a bitmask for behavior in Connection::cmd()
enum class Flag : uint8_t {
  // Flush previous responses or keep them
  FLUSH   = 0x1,
  PERSIST = 0x2,
  // Queue response or discard/ignore it
  QUEUE   = 0x4,
  DISCARD = 0x8,

  // Just another alias for DEFAULT
  NONE = 0x0,

  // PREDEFINED COMBINATIONS:
    // DEFAULT = 0x5
    // Flush old responses and queue these ones.
    //
    // For the most basic commands, this should be fine:
    //   cmd("set", "foo", 123);
    //   int foo = cmd("get", "foo");
    //
    DEFAULT = (FLUSH | QUEUE),

    // STASH = 0x6
    // Keep old responses and also queue these ones.
    //
    // Useful for situations such as this:
    //   cmd("lpush", "my_list", "foobat", "foobas", "foobar");
    //   cmd("lrange", "my_list", 0, -1);
    //   while (cmd<STASH>("rpop", "my_list") != "foobar") {...}
    //
    STASH = (PERSIST | QUEUE),

    // CLEAR = 0x9
    // Flush old responses, also ignore this one.
    //
    // Perhaps useful for commands that need a fresh queue before and afterward, or maybe you just don't
    //   care... or maybe initializing certain keys?... and whose responses can be
    //   safely ignored for some reason?...
    // Maybe for stuff like:
    //
    //   cmd<CLEAR>("select", 2);
    //
    // NOTE: Should be zero responses afterward.  Subsequent attempts to fetch a response will be in error.
    //   response() -> returns boost::none w/ error msg attached
    //
    CLEAR = (FLUSH | DISCARD),

    // VOID = 0xA
    // Keep old responses but discard these ones.
    //
    // Useful for commands that would disrupt the state of the response queue:
    //   cmd("lpush", "my_list", cmd("rpop", "other_list"));
    //   cmd("lrange", "my_list", 0, -1);
    //     You simply cannot wait and MUST delete other_list here:
    //   cmd<VOID>("del", "other_list")
    //   
    //   while(auto queued_response = response()) { do something with queued_response... }
    //
    VOID = (PERSIST | DISCARD),


  // ILLEGAL OPTIONS: contradictory
    ILLEGAL_FLUSH_OPTS = (FLUSH | PERSIST), // 0x3
    ILLEGAL_QUEUE_OPTS = (QUEUE | DISCARD), // 0xC
    // for completeness:
    ILLEGAL_FLUSH_OPTS_2 = (ILLEGAL_FLUSH_OPTS | QUEUE),   // 0x7
    ILLEGAL_FLUSH_OPTS_3 = (ILLEGAL_FLUSH_OPTS | DISCARD), // 0xB
    ILLEGAL_QUEUE_OPTS_2 = (ILLEGAL_QUEUE_OPTS | FLUSH),   // 0xD
    ILLEGAL_QUEUE_OPTS_3 = (ILLEGAL_QUEUE_OPTS | PERSIST), // 0xE
    ILLEGAL_OPTS_ALL     = (ILLEGAL_FLUSH_OPTS | ILLEGAL_QUEUE_OPTS) // 0xF
};


namespace flag { // {{{
using cmd::Flag;
using FType = std::underlying_type<Flag>::type;

template<Flag> struct IsLegal;
template<Flag> struct FlushResponses;
template<Flag> struct QueueResponses;

template<Flag T> struct IsLegal
    : std::integral_constant<bool, 
        static_cast<FType>(T) != (static_cast<FType>(T) & static_cast<FType>(Flag::ILLEGAL_FLUSH_OPTS)) &&
        static_cast<FType>(T) != (static_cast<FType>(T) & static_cast<FType>(Flag::ILLEGAL_QUEUE_OPTS))
    >
{};

template<Flag T> struct FlushResponses
    : std::integral_constant<bool, 
        !!(static_cast<FType>(T) & static_cast<FType>(Flag::FLUSH))
    >
{};

template<Flag T> struct QueueResponses
    : std::integral_constant<bool, 
        !!(static_cast<FType>(T) & static_cast<FType>(Flag::QUEUE))
      >
{};
} // namespace flag }}}

} // namespace cmd }}}


namespace CONSTANTS { // {{{
constexpr char const *NIL = "(nil)";
constexpr char const *OK  = "OK";

constexpr char const *UNKNOWN_STR = "";
constexpr int         UNKNOWN_INT = -1;

// Number of chars in hash string generated by Redis when Lua
//   or other scripts are digested and stored for reuse.
constexpr size_t SCRIPT_HASH_LENGTH = 40;
} // namespace rediswraps::CONSTANTS }}}

namespace DEFAULT { // {{{
constexpr char const *HOST = "127.0.0.1";
constexpr int         PORT = 6379;
} // namespace rediswraps::DEFAULT }}}


// rediswraps::utils {{{
namespace utils {

template<typename Token, typename = typename std::enable_if<std::is_constructible<std::string, Token>::value>::type, typename Dummy = void>
inline_
std::string to_string(Token const &item) {
  return item;
}

template<typename Token, typename = typename std::enable_if<!std::is_constructible<std::string, Token>::value>::type>
inline_
std::string to_string(Token const &item) {
  std::string converted;

  if (boost::conversion::try_lexical_convert(item, converted)) {
    return converted;
  }

  return "";
}


template<typename TargetType, typename = typename std::enable_if<std::is_default_constructible<TargetType>::value && !std::is_void<TargetType>::value>::type>
inline_
TargetType convert(std::string const &target) {
  TargetType new_target;

  if (boost::conversion::try_lexical_convert(target, new_target)) {
    return new_target;
  }

  return TargetType();
}

template<>
inline_
bool convert<bool>(std::string const &target) {
  return !target.empty() && (target != CONSTANTS::NIL) && (
    (target == CONSTANTS::OK) ||
    // In order to make strings literally containing "true" return true, uncomment the following:
    // (target == "true") ||
    (std::strtol(target.c_str(), nullptr, 10) != 0)
    // Case-insensitive version.  e.g. also convert "TrUe" to true:
    // || (target.length() == 4 &&
    //  (target[0] == 'T' || target[0] == 't') &&
    //  (target[1] == 'R' || target[1] == 'r') &&
    //  (target[2] == 'U' || target[2] == 'u') &&
    //  (target[3] == 'E' || target[3] == 'e')
    //)
  );
}


std::string const read_file(std::string const &filepath) {
  std::ifstream input(filepath);
  std::stringstream buffer;

  buffer << input.rdbuf();
  return buffer.str();
}

} // rediswraps::utils }}}


// Response classes {{{
//   Simple wrapper around std::string that adds an error check bool
class Response {
  //TODO why do I need to make Connection a friend?
  friend class Connection;

 // public {{{
 public:
  inline_
  Response() : success_(true) {}

  template<typename T>
  inline_
  Response(T data, bool success = true)
      : data_(utils::to_string(data)),
        success_(success)
  {}

  template<typename T>
  inline_
  operator T() const noexcept {
    return utils::convert<T>(this->data());
  }

  friend std::ostream& operator<<(std::ostream &os, Response const &response);

  // bool() -- two variants {{{
  // For L-vals
  inline_
  //TODO TRY explicit
  explicit operator bool() const& noexcept {
    return this->success();
  }

  // For R-vals
  inline_
  //TODO TRY explicit
  explicit operator bool() && noexcept {
    return this->success() && utils::convert<bool>(this->data());
  }
  // bool() - two variants }}}

  inline_
  operator void() const noexcept {}


  inline_
  std::string const data() const noexcept {
    return this->data_;
  }
  
  inline_
  bool success() const noexcept {
    return this->success_;
  }

  inline_
  bool operator ==(Response const &other) const noexcept {
    return (this == &other) ||
    (
      this->success() == other.success() &&
      this->data()    == other.data()
    );
  }
  
// Response comparison operators {{{
// operator ==
template<typename T>
inline_
friend bool operator ==(Response const &response, T const &other) {
  if (std::is_same<T, double>::value) {
    return response == float(other);
  }

  return utils::convert<T>(response.data()) == other;
}

template<typename T>
inline_
friend bool operator ==(T const &other, Response const &response) {
  if (std::is_same<T, double>::value) {
    return response == float(other);
  }

  return utils::convert<T>(response.data()) == other;
}

// operator <
template<typename T>
inline_
friend bool operator <(Response const &response, T const &other) {
  if (std::is_same<T, double>::value) {
    return response < float(other);
  }

  return utils::convert<T>(response.data()) < other;
}

template<typename T>
inline_
friend bool operator <(T const &other, Response const &response) {
  if (std::is_same<T, double>::value) {
    return float(other) < response;
  }

  return other < utils::convert<T>(response.data());
}

// operator !=
template<typename T>
inline_
friend bool operator !=(Response const &response, T const &other) {
  return !(response == other);
}

template<typename T>
inline_
friend bool operator !=(T const &other, Response const &response) {
  return !(other == response);
}

// opeartor >
template<typename T>
inline_
friend bool operator >(Response const &response, T const &other) {
  return !(response < other || response == other);
}

template<typename T>
inline_
friend bool operator >(T const &other, Response const &response) {
  return !(other < response || other == response);
}

// operator <=
template<typename T>
inline_
friend bool operator <=(Response const &response, T const &other) {
  return !(response > other);
}

template<typename T>
inline_
friend bool operator <=(T const &other, Response const &response) {
  return !(other > response);
}

// operator >=
template<typename T>
inline_
friend bool operator >=(Response const &response, T const &other) {
  return !(response < other);
}

template<typename T>
inline_
friend bool operator >=(T const &other, Response const &response) {
  return !(other < response);
}
// Response comparison operators }}}
 // public }}}

 // private {{{
 private:
  std::string data_;
  bool success_;

  template<typename T>
  inline_
  void set(T new_data) noexcept {
    this->data_ = utils::to_string(new_data);
  }

  inline_
  void fail() noexcept {
    this->success_ = false;
  }
 // private }}}
};
// Response }}}

inline_
std::ostream& operator<<(std::ostream &os, Response const &response) {
  return os << response.data();
}

class Connection { // {{{
  using ResponseQueueType = std::deque<std::string>;

// public interface {{{
 public:
  typedef std::unique_ptr<Connection> Ptr;

  // constructors & destructors {{{
  Connection(std::string const &host = DEFAULT::HOST, int const port = DEFAULT::PORT, std::string const &name = "")
      : socket_(boost::none),
        host_(boost::make_optional(!host.empty(), host)),
        port_(boost::make_optional(port > 0, port)),
        name_(boost::make_optional(!name.empty(), name))
  {
    this->connect();
  }

  Connection(std::string const &socket, std::string const &name = "")
      : socket_(boost::make_optional(!socket.empty(), socket)),
        host_(boost::none),
        port_(boost::none),
        name_(boost::make_optional(!name.empty(), name))
  {
    this->connect();
  }

  ~Connection() {
    this->disconnect();
  }
  // constructors & destructors }}}

  // responses_to_string()
  //   returns all responses in the queue at once, as one big \n-delimited string.
  // Useful for debugging.
  std::string responses_to_string() const {
    std::string desc;
    
    if (this->has_response()) {
      auto tmp_queue(this->responses_);

      for (int i = 0; i < tmp_queue.size(); ++i) {
        desc += "\n  [";
        desc += i;
        desc += "] => '";
        desc += tmp_queue.front();
        desc += "'";

        tmp_queue.pop_front();
      }
    }

    return desc;
  }
  
  friend std::ostream& operator<< (std::ostream &os, Connection const &conn);


  // description() {{{
  std::string description() const {
    std::string desc("Redis Connection {");
    
    desc += "\nName : "; desc += this->name();

    if (this->using_socket()) {
      desc += "\nSocket : "; desc += this->socket();
    }
    else if (this->using_host_and_port()) {
      desc += "\nHost : "; desc += this->host();
      desc += "\nPort : "; desc += utils::to_string(this->port());
    }

    desc += "\n\nResponse queue : ";
    desc += this->responses_to_string();

    desc += "\n}";
    return desc;
  }
  // description() }}}

  inline_
  void flush() {
    this->responses_ = {};
  }

  inline_
  bool has_response() const noexcept {
    return !this->responses_.empty();
  }

  inline_
  size_t const num_responses() const noexcept {
    return this->responses_.size();
  }

  inline_
  bool is_connected() const noexcept {
    return !(this->context_ == nullptr || this->context_->err);
  }

  inline_
  std::string name() const noexcept {
    return this->name_ ? *this->name_ : CONSTANTS::UNKNOWN_STR;
  }

  inline_
  std::string socket() const noexcept {
    return this->socket_ ? *this->socket_ : CONSTANTS::UNKNOWN_STR;
  }

  inline_
  std::string host() const noexcept {
    return this->host_ ? *this->host_ : CONSTANTS::UNKNOWN_STR;
  }

  inline_
  int port() const noexcept {
    return this->port_ && (*this->port_ > 0) ? *this->port_ : CONSTANTS::UNKNOWN_INT;
  }


  // load_script methods {{{
  //
  // Loads a script at either a filepath or from a string into Redis with a chosen alias.
  // This script must expect its first "keycount" number of arguments to be names of Redis keys.
  //
  // For example, if foo.lua expects one key name as its first argument, then:
  //
  //   this->load_lua_script("foo", "/path/to/foo.lua", 1);
  //
  // enables the following (assuming "bar" is some key name):
  //
  //   this->cmd("foo", "bar", 4, 1.23);
  //
  bool load_script_from_string(
      std::string const &alias,
      std::string const &script_contents,
      size_t const keycount = 0,
      bool flush_old_scripts = false
  ) {
    static bool okay_to_flush = true;

    if (flush_old_scripts) {
      if (okay_to_flush) {
        if (this->cmd("SCRIPT", "FLUSH")) {
          okay_to_flush = false;
        }
        else {
          std::cerr << "Warning: Couldn't flush old Lua scripts from Redis." << std::endl;
        }
      }
      else {
        std::cerr << "Warning: Scripts previously stored in Redis have already been flushed.  "
          "There is no need to do it again."
        << std::endl;
      }
    }

    std::string const script_hash = this->cmd("SCRIPT", "LOAD", script_contents);

    if (script_hash.empty()) {
      return false;
    }

    if (script_hash.length() != CONSTANTS::SCRIPT_HASH_LENGTH) {
      std::cerr << "Error: Could not properly load Lua script '" << alias << "' into Redis: invalid hash length." << std::endl;
      return false;
    }

    this->scripts_.emplace(
      alias,
      std::pair<std::string, size_t>(
        script_hash,
        keycount
      )
    );

    return true;
  }

  inline_
  bool load_script_from_file(
      std::string const &alias,
      std::string const &filepath,
      size_t const keycount = 0,
      bool flush_old_scripts = false
  ) {
    return this->load_script_from_string(alias, utils::read_file(filepath), keycount, flush_old_scripts);
  }

  // shorter alias for the filepath version:
  inline_
  bool load_script(
    std::string const &alias,
    std::string const &filepath,
    size_t const keycount = 0,
    bool flush_old_scripts = false
  ) {
    return this->load_script_from_file(alias, filepath, keycount, flush_old_scripts);
  }
  // load_script methods }}}


  // cmd() {{{
  //   Sends Redis a command.
  // The first argument is the command itself (e.g. "SETEX") and thus must be a string.
  //   All subsequent arguments will be converted to string automatically before being sent
  //   to Redis.
  //
  // The template argument is a bitmask enum for which you can select options.
  // See enum class CmdOpt above for the actual definition.
  //
  // There are really only two settings:
  // 1)  QUEUE or DISCARD any responses to this command
  // 2)  FLUSH or PERSIST any previously queued responses before issuing this command
  //
  // The default options are (QUEUE | FLUSH) to accomodate the basic command call:
  //   std::string foo = redis->cmd(..);
  //
  //   which would need to queue the response (so it can be assigned to foo)
  //   and flush any old responses (so foo receives the value it looks like it would)
  //
  // A compile-time error will be raised if either:
  //   Both DISCARD and QUEUE are set
  //   Both PERSIST and FLUSH are set
  //
  template<cmd::Flag flags = cmd::Flag::DEFAULT, typename RetType = Response, typename... Args>
  inline_
  RetType cmd(std::string const &base, Args&&... args) noexcept {
    static_assert(cmd::flag::IsLegal<flags>::value,
      "Illegal combination of cmd::Flag values."
    );
    
    if (cmd::flag::FlushResponses<flags>::value) {
      this->flush();
    }

    Response response = this->scripts_.count(base) ?
      this->cmd_proxy<flags>(
        "EVALSHA",
        this->scripts_[base].first,
        this->scripts_[base].second,
        std::forward<Args>(args)...
      )
    : this->cmd_proxy<flags>(base, std::forward<Args>(args)...);

    return static_cast<RetType>(response);
  }
  // cmd() }}}


  // response() {{{
  Response response(bool pop_response = true, bool from_front = false) const noexcept {
    if (pop_response && from_front) {
      std::cerr << "WARNING: You are popping from the front of the Redis response queue.  "
        "This is not recommended.  See RedisWraps README for more details on why this is dangerous."
      << std::endl;
    }

    if (!this->has_response()) {
      return Response("Redis has not previously queued any further responses.", false);
    }

    Response response(from_front ? this->responses_.front() : this->responses_.back());

    if (pop_response) {
      if (from_front) {
        this->responses_.pop_front();
      }
      else {
        this->responses_.pop_back();
      }
    }

    return response;
  }


  template<typename RetType, typename = typename std::enable_if<!std::is_same<RetType, Response>::value>::type>
  inline
  RetType response(bool pop_response = true, bool from_front = false) const noexcept {
    return static_cast<RetType>(this->response(pop_response, from_front));
  }
  // response() }}}

  // last_response() {{{
  //   Returns the most recent Redis response instead of the oldest, which is what response() returns.
  // Does not pop that response from the front by default.
  // Useful for debugging.
  //
  inline_
  Response const last_response(bool pop_response = false) const {
    return this->response(pop_response, true);
  }
  // last_response() }}}
// public interface }}}


// private {{{
 private:
  inline_
  bool using_socket() const noexcept {
    return !!this->socket_;
  }

  inline_
  bool using_host_and_port() const noexcept {
    return (!!this->host_) && (!!this->port_);
  }


  // dis/re/connect() {{{
  void connect() {
    if (!this->is_connected()) {
      // sockets are fastest, try that first
      if (this->using_socket()) {
        this->context_ = redisConnectUnix(this->socket().c_str());
      }
      else if (this->using_host_and_port()) {
        this->context_ = redisConnect(this->host().c_str(), this->port());
      }

      if (!this->is_connected()) {
        throw std::runtime_error(
          this->description() +
            (this->context_ == nullptr ?
              "Unknown error connecting to Redis" :
              this->context_->errstr
            )
        );
      }

      if (this->name_) {
        this->cmd<cmd::Flag::CLEAR>("CLIENT", "SETNAME", this->name());
      }
    }
  }

  inline_
  void disconnect() noexcept {
    if (this->is_connected()) {
      redisFree(this->context_);
    }

    this->context_ = nullptr;
  }

  inline_
  void reconnect() {
    this->disconnect();
    this->connect();
  }
  // dis/connect() }}}
  

  // parse_reply() {{{
  template<cmd::Flag flags>
  Response parse_reply(redisReply *&reply, bool const recursion = false) {
    Response response;
    
    // There is a corner case where we never want to stash the response:
    //   when reply->type is REDIS_REPLY_ARRAY
    bool is_array_reply = false;

    if (reply == nullptr || this->context_ == nullptr || this->context_->err) {
      response.fail();

      if (reply == nullptr) {
        response.set("Redis reply is null");
      }
      else {
        if (this->context_ == nullptr) {
          response.set("Not connected to Redis");
        }
        else {
          response.set(this->context_->errstr);
        }

        this->reconnect();
      }
    }
    else {
      switch(reply->type) {
      case REDIS_REPLY_ERROR:
        std::cerr << reply->str << std::endl;
        response.fail();
      case REDIS_REPLY_STATUS:
      case REDIS_REPLY_STRING:
        response.set(reply->str);
        break;
      case REDIS_REPLY_INTEGER:
        response.set(reply->integer);
        break;
      case REDIS_REPLY_NIL:
        response.set(CONSTANTS::NIL);
        break;
      case REDIS_REPLY_ARRAY:
        // Do not queue THIS reply... which is just to start the array unrolling and
        //   carries no actual reply data with it.
        // Recursive calls in the following for loop will not enter this section (unless
        //   they too are arrays...) and will not be affected
        is_array_reply = true;

        for (size_t i = 0; i < reply->elements; ++i) {
          Response tmp;
          if (!(tmp = this->parse_reply<flags>(reply->element[i], true))) {
            while (i-- > 0) {
              this->responses_.pop_back();
            }
            break;
          }
        }
        break;
      default:
        response.fail();
      }
    }

    if (!is_array_reply && cmd::flag::QueueResponses<flags>::value) {
      this->responses_.emplace_front(response.data());
    }

    if (!recursion) {
      freeReplyObject(this->reply_);
    }

    return response;
  }
  // parse_reply() }}}


  // format_cmd_args() {{{
  template<int argc>
  inline_
  void format_cmd_args(
    std::array<char*, argc> &&arg_strings,
    int const args_index
  ) {}

  template<int argc, typename Arg, typename... Args>
  void format_cmd_args(
    std::array<char*, argc> &&arg_strings,
    int const args_index,
    Arg const &arg,
    Args&&... args
  ) {
    this->format_cmd_args<argc>(
      std::forward<std::array<char*, argc>>(arg_strings),
      (args_index + 1),
      std::forward<Args>(args)...
    );

    std::string const temp(utils::to_string(arg));

    arg_strings[args_index] = new char[temp.size() + 1];
    strcpy(arg_strings[args_index], temp.c_str());
  }
  // format_cmd_args() }}}


  // cmd_proxy() {{{
  template<cmd::Flag flags, typename... Args>
  Response cmd_proxy(Args&&... args) {
    constexpr int argc = sizeof...(args);

    std::array<char*, argc> arg_strings;

    // TODO fix this to use RAII instead of new and delete[] on char arrays
    this->format_cmd_args<argc>(
      std::forward<std::array<char*, argc>>(arg_strings),
      0,
      std::forward<Args>(args)...
    );

    // if it fails maybe it disconnected?... try once to reconnect quickly before giving up
    bool reconnection_attempted = false;
    do {
      this->reply_ = reinterpret_cast<redisReply*>(
        redisCommandArgv(this->context_, argc, const_cast<char const**>(arg_strings.data()), nullptr)
      );

      if (this->reply_ != nullptr) {
        break;
      }

      if (reconnection_attempted) {
        return Response(
          this->context_->err ?
            this->context_->errstr :
            "Redis reply is null and reconnection failed.",
          false
        );
      }

      this->reconnect();
      reconnection_attempted = true;
    }
    while (this->reply_ == nullptr);
    
    auto response = this->parse_reply<flags>(this->reply_);

    for (int i = 0; i < argc; ++i) {
      delete[] arg_strings[i];
    }

    return response;
  }
  // cmd_proxy() }}}


  // member variables {{{
  boost::optional<std::string> socket_;
  boost::optional<std::string> host_;
  boost::optional<int>         port_;
  boost::optional<std::string> name_;

  redisContext *context_ = nullptr;
  redisReply   *reply_   = nullptr;

  mutable ResponseQueueType responses_ = {};

  // scripts_ maps the name of the lua script to the sha hash and the # of
  //   keys the script expects
  std::unordered_map<std::string, std::pair<std::string, size_t>> scripts_ = {};
  // member variables }}}
// private }}}
};
// class Connection }}}


inline_ std::ostream& operator<< (std::ostream &os, Connection const &conn) {
  return os << conn.description();
}

} // namespace rediswraps

#endif

