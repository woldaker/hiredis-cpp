<img align="left" src="logo.png" />

# RedisWraps
### A C++ library containing a simple and intuitive interface for Redis.
<br/>

## Prerequisites
##### NOTE: CMake is configured to automatically attempt resolution of all dependencies listed below which are listed *after* CMake itself.
- Compiler with C++11 support
- [CMake](https://cmake.org/)
- [hiredis](https://github.com/redis/hiredis)
- [Boost](http://www.boost.org/) (specifically [boost::lexical\_cast](http://www.boost.org/doc/libs/release/libs/lexical_cast/) and [boost::optional](http://www.boost.org/doc/libs/release/libs/optional/))

## How to use it
#### Include header and create a connection

```C++
#include <rediswraps/rediswraps.hh>
using Redis = rediswraps::Connection;

std::unique_ptr<Redis> redis;

// Construct with ("IP", port) or ("path/to/socket")
try {
	redis.reset(new Redis("12.34.56.78", 6379));
} catch (std::exception const &e) {
	std::cerr << e.what() << std::endl;
  /* handle error */
}
```

#### Issue commands with Cmd("name", args...)

Args may be a string (char\*, std::string), any fundamental type (from type\_traits), or any type which defines implicit conversion to std::string.

```C++
struct Foo {
	operator std::string() const { return "foo"; }
};
Foo myfoo;

redis->Cmd("mset", myfoo, 123, "bar", 4.56, "gaz", false);
```

### Getting responses
NOTE: The following examples assume that Redis still contains only {foo=123, bar=4.56, gaz=false}
#### Option 1: Use **Cmd( )** as an R-value
When used this way, **Cmd( )** will produce the same error result AND-ed with the actual Redis value:
```C++
if (redis->Cmd("get", "gaz")) {/*...*/}  // false as expected.
if (redis->Cmd("gert", "gaz")) {/*...*/} // false due to invalid command error.
  // prints to stderr: "ERR unknown command 'gert'"

// A concise way to get around this is using an auto variable inline:
if (auto gazval = redis->Cmd("get", "gaz")) { // true
  gazval.boolean(); // false
}
```

#### Option 2: Assign the output of **Cmd( )** to a variable
When used this way, the value produced is the same as before with the exception of booleans.
```C++
int   foo = redis->Cmd("get", "foo");
float bar = redis->Cmd("get", "bar");
bool  gaz = redis->Cmd("get", "gaz");

foo == 123;  // true
bar == 4.56; // true
gaz == true; // true!!  See note below
```

##### IMPORTANT NOTE ABOUT BOOLEAN RESPONSES
Assigning to boolean will not produce the boolean value of the Redis data but *whether or not the command executed correctly*.
To get exactly the behavior you want, use a combination of **auto** and the resulting object's **boolean( )** and/or **success( )** methods, as shown below.

```C++
auto gazval = redis->Cmd("get", "gaz");
auto errval = redis->Cmd("gert", "gaz");
  // prints to stderr: "ERR unknown command 'gert'"

gazval;           // true  : "get gaz" is a valid command.
gazval.success(); // true  : same reason.
gazval.boolean(); // false : key "gaz" has a false value.
errval;           // false : "gert gaz" is not a valid command.
errval.success(); // false : same reason.
errval.boolean(); // false : same reason.  Value of "gaz" never fetched.
```

#### Option 3: Loop through batch replies with **Response( )**
```C++
redis->Cmd("rpush", "mylist", 1, "2", 3.4);
redis->Cmd("lrange", "mylist", 0, -1);

// WARNING: Any calls to Cmd() here will destroy the results of the previous lrange call!
// See below for a way around this.

while (auto listval = redis->Response()) {
	std::cout << listval << std::endl;
}

// Prints:
// 1
// 2
// 3.4
```

as an alternative, you may use the **HasResponse( )** method:

```C++
redis->Cmd("lrange", "mylist", 0, -1);

while (redis->HasResponse()) {
	std::cout << redis->Response() << std::endl;
}
```

or if you wish to top a response (and not pop it), pass false as an argument:

```C++
auto next_response_peek = redis->Response(false);
```

### Changing the behavior of **Cmd( )**
Cmd( ) may take template arguments which will modify the way it handles calls and responses.
These arguments must be of type **rediswraps::cmd::Flag**.
To make this easier on programmers, four constants were created within the rediswraps namespace
which alias values of rediswraps::cmd::Flag, as illustrated below.

EXAMPLE NOTE: For all of the following cases, replace ??? with the corresponding template argument.
```C++
// In Redis: foo=123
redis->Cmd("get", "foo");
auto fooval = redis->Cmd<???>("set", "foo", 456);

std::cout << fooval << std::endl;

if (auto next_response = redis->Response()) {
  std::cout << next_response << std::endl;
}
else {
  std::cout << "No further responses!" << std::endl;
}
```


#### Cmd( )              *<-- normal version.*
#### Cmd<CMD_DEFAULT>( ) *<-- verbose version.  Unnecessary.  Listed here just for completeness.*
Flushes all previous responses from the response queue.
Queues new response(s).
This is the default behavior resulting from providing no template arguments.
```
Example prints:
OK
No further responses!
```

#### Cmd<CMD_SAVED>( )
Saves all previous responses in the response queue.
Queues new response(s).
```
Example prints:
123
OK
```

#### Cmd<CMD_VOID>( )
Saves all previous responses in the response queue.
Does not queue response(s) from queue.
```
Example prints:
123
No further responses!
```

#### Cmd<CMD_CLEAR>( )
Flushes all previous responses from the response queue.
Does not queue response(s) from queue.
```
Example prints:
Redis has not previously queued any further responses.
No further responses!
```
NOTE: The first message comes from connection.cc due to a false response.


### Load new commands using Lua:
Use either **LoadScriptFromFile( )** or **LoadScript( )** (the latter is an alias for the former):

```C++
redis->Cmd("gert", "mylist", "foo");
// ERR unknown command 'gert'

redis->LoadScript("gert", "/path/to/script.lua",
	1   // number of KEYS[] Lua will expect; default = 0.
);

redis->Cmd("gert", "mylist", "foo");   // Now it will work
```

Or use **LoadScriptFromString( )**:

```C++
redis->LoadScriptFromString("pointless", 
	"return redis.call('ECHO', 'This command is pointless!')"
);

redis->Cmd("pointless");
// prints "This command is pointless!"
```


## Build
When building an object that uses it:
`g++`**`-std=c++11`**`-c your_obj.cc -o your_obj.o`

When linking a binary that uses it:
`g++`**`-std=c++11`**`your_program.cc -o YourProgram`**`-lrediswraps`**


## TODO
This project is very young and has quite a few features that are still missing.
Here are just a few off the top of my head:

- Much more testing needs to be written.
- Async calls.  Original solution used [libev](http://software.schmorp.de/pkg/libev.html).
- Pubsub support.  The original code I wrote, repurposed here as RedisWraps, used a combination of [boost::lockfree::spsc\_queue](http://www.boost.org/doc/libs/release/doc/html/boost/lockfree/spsc_queue.html) and a simple "event" struct to shove into the queue for this purpose.  Inherently requires multithreading and, if I remember the implementation correctly, the async TODO as prerequisites.
- Cluster & slave support.  I actually know very little about this topic in general.
- Untested on Windows.  CMake build system will almost certainly not work there.  The library itself, however, doesn't use any Unix-specific headers that I'm aware of.
- Hardcoded command methods e.g. redis->rpush(...) (Is this really a good idea?)

## Authors

* **Wesley Oldaker** - *Initial work* - [WesleyOldaker](https://github.com/woldaker)

## License

This project is licensed under the [BSD 3-clause license](LICENSE).

