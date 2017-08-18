#ifndef HIREDIS_CPP_HH
#define HIREDIS_CPP_HH

#include <cstring> // strcpy() used in format_cmd_args()

#include <array>         // used in cmd_proxy()
#include <deque>         // Holds all the response strings from Redis
#include <iostream>
#include <fstream>       // used in read_file()
#include <sstream>       // used in read_file()
#include <string>
#include <type_traits>   // enable_if<>...
#include <unordered_map> // Maps Lua scripts to their hash digests.
#include <utility>       // std::forward()

#include <boost/lexical_cast.hpp>
#include <boost/optional.hpp>

extern "C" {
#include <hiredis/hiredis.h>
}


namespace hiredis_cpp {
namespace DEFAULT {
  constexpr char const *HOST = "127.0.0.1";
  constexpr int         PORT = 6379;
}

constexpr char const *NIL = "(nil)";
constexpr char const *OK  = "OK";

// Number of chars in hash string generated by Redis when Lua
//   or other scripts are digested and stored for reuse.
constexpr size_t SCRIPT_HASH_LENGTH = 40;

// Dummy types used in cmd() to force responses into a queue for later use
struct Stash {};
struct Void  {};

namespace utils {
template<typename Token, typename = typename std::enable_if<std::is_constructible<std::string, Token>::value>::type>
inline std::string to_string(Token const &item) {
  return item;
}

template<typename Token, typename = typename std::enable_if<!std::is_constructible<std::string, Token>::value>::type, typename Dummy = void>
inline std::string to_string(Token const &item) {
  std::string converted;

  return boost::conversion::try_lexical_convert(item, converted) ?
    converted :
    (std::cerr << "Conversion ERROR:\n" << __PRETTY_FUNCTION__ << std::endl, "");
}

template<typename TargetType, typename = typename std::enable_if<std::is_default_constructible<TargetType>::value>::type>
inline TargetType convert(std::string const &target) {
  TargetType new_target;

  return boost::conversion::try_lexical_convert(target, new_target) ?
    new_target :
    (std::cerr << "Conversion ERROR:\n" << __PRETTY_FUNCTION__ << std::endl, TargetType());
}

template<>
inline Stash convert<Stash>(std::string const &target) {
  return {};
}

template<>
inline Void convert<Void>(std::string const &target) {
  return {};
}

template<>
bool convert<bool>(std::string const &target) {
  return !target.empty() && (target != NIL) && (
    (target == OK) ||
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
} // namespace utils


class Connection {
 public:
  // constructors & destructors {{{
  Connection(std::string const &host = DEFAULT::HOST, int const port = DEFAULT::PORT)
      : socket_(boost::none),
        host_(host),
        port_(port)
  {
    this->connect();
  }

  Connection(std::string const &socket)
      : socket_(socket),
        host_(boost::none),
        port_(boost::none)
  {
    this->connect();
  }

  ~Connection() {
    this->disconnect();
  }
  // constructors & destructors }}}

  // small, inline methods - flush, has_response, & is_connected {{{
  inline void flush() {
    this->responses_ = {};
  }

  inline bool const has_response() {
    return !this->responses_.empty();
  }

  inline bool const is_connected() const noexcept {
    return !(this->context_ == nullptr || this->context_->err);
  }
  // flush, has_response, & is_connected }}}


  // load_lua_script methods {{{
  //
  // Loads a Lua script at either a filepath or from a string into Redis with a chosen alias.
  // This script must expect its first "keycount" number of arguments to be names of Redis keys.
  //
  // For example, if foo.lua expects one key name as its first argument, then:
  //
  //   this->load_lua_script("/path/to/foo.lua", "foo", 1);
  //
  // enables the following (assuming "bar" is some key name):
  //
  //   this->cmd("foo", "bar", 4, 1.23);
  //
  bool const load_lua_script_from_string(
      std::string const &script_contents,
      std::string const &alias,
      size_t const keycount = 0,
      bool const flush_old_scripts = false
  ) {
    static bool okay_to_flush = true;

    if (flush_old_scripts) {
      if (okay_to_flush) {
        if (this->cmd<bool>("SCRIPT", "FLUSH")) {
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

    auto script_hash = this->cmd<std::string>("SCRIPT", "LOAD", script_contents);
    if (!script_hash) {
      return false;
    }

    if (script_hash->length() != SCRIPT_HASH_LENGTH) {
      std::cerr << "Error: Could not properly load Lua script '" << alias << "' into Redis: invalid hash length." << std::endl;
      return false;
    }

    this->scripts_.emplace(alias, std::pair<std::string, size_t>(*script_hash, keycount));
    return true;
  }

  inline bool const load_lua_script_from_file(
      std::string const &filepath,
      std::string const &alias,
      size_t const keycount = 0,
      bool const flush_old_scripts = false
  ) {
    return this->load_lua_script_from_string(utils::read_file(filepath), alias, keycount, flush_old_scripts);
  }

  // shorter alias for the filepath version:
  inline bool const load_lua_script(
    std::string const &filepath,
    std::string const &alias,
    size_t const keycount = 0,
    bool const flush_old_scripts = false
  ) {
    return this->load_lua_script_from_file(filepath, alias, keycount, flush_old_scripts);
  }
  // load_lua_script methods }}}


  // cmd() {{{
  //
  // IMPORTANT :
  //  Template parameter ReturnType set to type 'Stash', defined as an empty dummy struct
  //    atop this file, has one special difference than using any other type:
  //    Responses will be pushed onto a queue instead of being returned directly to the caller.
  //
  //    No matter what, a boost::optional<> will be returned so you can check if an error occurred.
  //
  // Examples:
  //  Typical use case:
  //
  //  auto foo = redis->cmd<int>("get", "foo");
  //  if (foo && *foo == 123) ...
  //
  //  Using type 'Stash' (necessary in order for the following to work):
  //
  //  if (redis->cmd<Stash>("lrange", "foo", 0, -1)) {
  //    int foo_val;
  //
  //    while (redis->has_response()) {
  //      foo_val = redis->response<int>();
  //      ... do stuff with foo_val perhaps ...
  //    }
  //  }
  //
  //  If Stash were not used in the last example, all the values we fetched from the list 'foo'
  //    would be discarded and we would only be returned the last one.
  //
  template<typename ReturnType = std::string, typename = typename std::enable_if<!std::is_void<ReturnType>::value>::type, typename... Args>
  inline boost::optional<ReturnType> cmd(std::string const &base, Args&&... args) noexcept {
    return this->scripts_.count(base) ?
      this->cmd_proxy<ReturnType>(
        "EVALSHA",
        this->scripts_[base].first,
        this->scripts_[base].second,
        std::forward<Args>(args)...
      )
    : this->cmd_proxy<ReturnType>(
        base,
        std::forward<Args>(args)...
      );
  }
  
  // Cmd()
  //
  // Just like cmd() except returns the actual type OR throws an exception
  template<typename ReturnType = Void, typename = typename std::enable_if<!std::is_void<ReturnType>::value>::type, typename... Args>
  inline ReturnType Cmd(std::string const &base, Args&&... args) {
    auto retval = this->cmd<ReturnType>(base, std::forward<Args>(args)...);

    if (!retval) {
      throw std::logic_error("");
    }

    return *retval;
  }
  // cmd() }}}


  // response() {{{
  template<typename ReturnType = std::string>
  boost::optional<ReturnType> response(bool const pop_response = true, bool const from_back = false) {
    if (!this->has_response()) {
      std::cerr << "Warning: No responses left in queue." << std::endl;
      return boost::none;
    }

    boost::optional<ReturnType> retval = 
      boost::make_optional(true,
        utils::convert<ReturnType>(from_back ?
          this->responses_.back() :
          this->responses_.front()
        )
      );

    if (pop_response) {
      if (from_back) {
        this->responses_.pop_back();
      }
      else {
        this->responses_.pop_front();
      }
    }

    return retval;
  }

  // Same as response() except returns the actual value or throws an exception
  template<typename ReturnType = std::string>
  inline ReturnType Response(bool const pop_response = true, bool const from_back = false) {
    auto retval = this->response<ReturnType>(pop_response, from_back);

    if (!retval) {
      throw std::runtime_error("");
    }

    return *retval;
  }
  // response() }}}

  // last_response() {{{
  template<typename ReturnType = std::string>
  inline boost::optional<ReturnType> last_response(bool const pop_response) {
    return this->response<ReturnType>(pop_response, true);
  }
  // last_response() }}}
  


  // print_responses() {{{
  void print_responses() const& {
    std::cout << "{";

    if (this->responses_.empty()) {
      std::cout << " }" << std::endl;
      return;
    }

    int i = 0;
    for (auto const &response : this->responses_) {
      std::cout << "\n\t[" << i++ << "] => '" << response << "'";
    }

    std::cout << "\n}" << std::endl;
  }
  // print_responses() }}}

 private:
  // dis/re/connect() {{{
  void connect() {
    if (!this->is_connected()) {
      // sockets are fastest, try that first
      if (this->socket_) {
        this->context_ = redisConnectUnix(this->socket_->c_str());
      }
      else if (this->host_ && this->port_) {
        this->context_ = redisConnect(this->host_->c_str(), *this->port_);
      }

      if (!this->is_connected()) {
        throw std::runtime_error(
          this->context_ == nullptr ?
            "Unknown error connecting to Redis" :
            this->context_->errstr
        );
      }
    }
  }

  void disconnect() noexcept {
    if (this->is_connected()) {
      redisFree(this->context_);
    }

    this->context_ = nullptr;
  }

  inline void reconnect() {
    this->disconnect();
    this->connect();
  }
  // dis/connect() }}}
  
  // parse_reply() {{{
  template<typename ReturnType, typename = typename std::enable_if<!std::is_void<ReturnType>::value>::type>
  boost::optional<ReturnType> parse_reply(redisReply *&reply, bool const recursion = false) {
    constexpr bool queue_responses = std::is_same<ReturnType, Stash>::value;

    std::string response_string;
    bool success = true;

    if (reply == nullptr || this->context_ == nullptr || this->context_->err) {
      success = false;

      if (reply == nullptr) {
        response_string = "Warning: Redis reply is null";
      }
      else {
        if (this->context_ == nullptr) {
          response_string = "Error: Not connected to Redis";
        }
        else {
          response_string = this->context_->errstr;
        }

        this->reconnect();
      }
    }
    else {
      switch(reply->type) {
      case REDIS_REPLY_ERROR:
        success = false;
        break;
      case REDIS_REPLY_STATUS:
      case REDIS_REPLY_STRING:
        response_string = reply->str;
        break;
      case REDIS_REPLY_INTEGER:
        response_string = utils::to_string(reply->integer);
        break;
      case REDIS_REPLY_NIL:
        response_string = NIL;
        break;
      case REDIS_REPLY_ARRAY:
        for (size_t i = 0; i < reply->elements; ++i) {
          success &= !!this->parse_reply<ReturnType>(reply->element[i], true);

          // if there is an error remove all the successful responses that were pushed
          //  onto the response queue before the error occurred.
          if (!success) {
            if (queue_responses) {
              while (i-- > 0) {
                this->responses_.pop_back();
              }
            }

            break;
          }
        }
        break;
      default:
        success = false;
      }
    }

    if (success && queue_responses) {
      this->responses_.emplace_back(response_string);
    }

    if (!recursion) {
      if (!success) {
        std::cerr << response_string << std::endl;
      }

      freeReplyObject(this->reply_);
    }

    return boost::make_optional(success, utils::convert<ReturnType>(response_string));
  }
  // parse_reply() }}}

  // format_cmd_args() {{{
  template<int argc>
  inline void format_cmd_args(
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
  template<typename ReturnType, typename = typename std::enable_if<!std::is_void<ReturnType>::value>::type, typename... Args>
  boost::optional<ReturnType> cmd_proxy(Args&&... args) {
    constexpr int argc = sizeof...(args);

    std::array<char*, argc> arg_strings;

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
        std::cerr << (this->context_->err ?
          this->context_->errstr :
          "Redis reply is null and reconnection failed."
        ) << std::endl;

        return boost::none;
      }

      this->reconnect();
      reconnection_attempted = true;
    }
    while (this->reply_ == nullptr);
    
    boost::optional<ReturnType> retval = this->parse_reply<ReturnType>(this->reply_);

    for (int i = 0; i < argc; ++i) {
      delete[] arg_strings[i];
    }

    return retval;
  }
  // cmd_proxy() }}}

  // member variables:
  boost::optional<std::string> socket_;
  boost::optional<std::string> host_;
  boost::optional<int>         port_;

  redisContext *context_ = nullptr;
  redisReply   *reply_   = nullptr;

  std::deque<std::string> responses_ = {};

  // scripts_ maps the name of the lua script to the sha hash and the # of
  //   keys the script expects
  std::unordered_map<std::string, std::pair<std::string, size_t>> scripts_ = {};
};

} // namespace hiredis_cpp

using hiredis_cpp::Stash;

#endif

