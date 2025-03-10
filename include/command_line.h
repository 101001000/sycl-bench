#ifndef BENCHMARK_COMMAND_LINE_HPP
#define BENCHMARK_COMMAND_LINE_HPP

#include "common.h"

#include "result_consumer.h"
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sycl/sycl.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using CommandLineArguments = std::unordered_map<std::string, std::string>;
using FlagList = std::unordered_set<std::string>;

namespace detail {

template <class T>
inline T simple_cast(const std::string& s) {
  std::stringstream sstr{s};
  if constexpr (std::is_same_v<T, std::string>) {
        std::string result;
        std::getline(sstr, result, '\0'); // read until EOF
        return result;
    } else {
        T result;
        sstr >> result;
        return result;
    }
}

template <class T>
inline std::vector<T> parseCommaDelimitedList(const std::string& s) {
  std::stringstream istr(s);
  std::string current;
  std::vector<T> result;

  while(std::getline(istr, current, ',')) result.push_back(simple_cast<T>(current));

  return result;
}

template <class SyclArraylike>
inline SyclArraylike parseSyclArray(const std::string& s, std::size_t defaultValue) {
  auto elements = parseCommaDelimitedList<std::size_t>(s);
  if(s.size() > 3)
    throw std::invalid_argument{"Invalid sycl range/id: " + s};
  else if(s.size() == 3)
    return SyclArraylike{elements[0], elements[1], elements[2]};
  else if(s.size() == 2)
    return SyclArraylike{elements[0], elements[1], defaultValue};
  else if(s.size() == 1)
    return SyclArraylike{elements[0], defaultValue, defaultValue};
  else
    throw std::invalid_argument{"Invalid sycl range/id: " + s};
}

} // namespace detail

template <class T>
inline T cast(const std::string& s) {
  return detail::simple_cast<T>(s);
}

template <>
inline sycl::range<3> cast(const std::string& s) {
  return detail::parseSyclArray<sycl::range<3>>(s, 1);
}

template <>
inline sycl::id<3> cast(const std::string& s) {
  return detail::parseSyclArray<sycl::id<3>>(s, 0);
}

class CommandLine {
public:
  CommandLine() = default;

  CommandLine(int argc, char** argv) {
    for(int i = 0; i < argc; ++i) {
      std::string arg = argv[i];
      auto pos = arg.find("=");
      if(pos != std::string::npos) {
        auto argName = arg.substr(0, pos);
        auto argVal = arg.substr(pos + 1);

        if(args.find(argName) != args.end()) {
          throw std::invalid_argument{"Encountered command line argument several times: " + argName};
        }
        args[argName] = argVal;
      } else {
        flags.insert(arg);
      }
    }
  }

  bool isArgSet(const std::string& arg) const { return args.find(arg) != args.end(); }

  template <class T>
  T getOrDefault(const std::string& arg, const T& defaultVal) const {
    if(isArgSet(arg))
      return cast<T>(args.at(arg));
    return defaultVal;
  }

  template <class T>
  T get(const std::string& arg) const {
    try {
      return cast<T>(args.at(arg));
    } catch(std::out_of_range& e) {
      throw std::invalid_argument{"Command line argument was requested but missing: " + arg};
    }
  }

  bool isFlagSet(const std::string& flag) const { return flags.find(flag) != flags.end(); }


private:
  CommandLineArguments args;
  FlagList flags;
};


struct VerificationSetting {
  bool enabled;
  sycl::id<3> begin = {0, 0, 0};
  sycl::range<3> range = {1, 1, 1};
};

struct BenchmarkArgs {
  size_t problem_size;
  size_t local_size;
  size_t num_runs;
  sycl::queue device_queue;
  sycl::queue device_queue_in_order;
  VerificationSetting verification;
  // can be used to query additional benchmark specific information from the command line
  CommandLine cli;
  std::shared_ptr<ResultConsumer> result_consumer;
  bool warmup_run;
};


class BenchmarkCommandLine {
public:
  BenchmarkCommandLine(int argc, char** argv) : cli_parser{argc, argv} {}

  BenchmarkArgs getBenchmarkArgs() const {
    std::size_t size = cli_parser.getOrDefault<std::size_t>("--size", 3072);
    std::size_t local_size = cli_parser.getOrDefault<std::size_t>("--local", 256);
    std::size_t num_runs = cli_parser.getOrDefault<std::size_t>("--num-runs", 5);

    std::string device_type = cli_parser.getOrDefault<std::string>("--device", "default");
    sycl::queue q = getQueue(device_type);
    sycl::queue q_in_order = getQueue(device_type, sycl::property::queue::in_order{});

    bool verification_enabled = true;
    if(cli_parser.isFlagSet("--no-verification"))
      verification_enabled = false;

    auto verification_begin = cli_parser.getOrDefault<sycl::id<3>>("--verification-begin", sycl::id<3>{0, 0, 0});

    auto verification_range = cli_parser.getOrDefault<sycl::range<3>>("--verification-range", sycl::range<3>{1, 1, 1});

    auto result_consumer = getResultConsumer(cli_parser.getOrDefault<std::string>("--output", "stdio"));

    return BenchmarkArgs{size, local_size, num_runs, q, q_in_order,
        VerificationSetting{verification_enabled, verification_begin, verification_range}, cli_parser, result_consumer};
  }

private:
  std::shared_ptr<ResultConsumer>

  getResultConsumer(const std::string& result_consumer_name) const {
    if(result_consumer_name == "stdio")
      return std::shared_ptr<ResultConsumer>{new OstreamResultConsumer{std::cout}};
    else
      // create result consumer that appends to a csv file, interpreting the output name
      // as the target file name
      return std::shared_ptr<ResultConsumer>{new AppendingCsvResultConsumer{result_consumer_name}};
  }

  template <typename... Props>
  sycl::queue getQueue(const std::string& device_type, Props&&... props) const {
    const auto getQueueProperties = [&]() -> sycl::property_list {

#if defined(SYCL_BENCH_ENABLE_QUEUE_PROFILING)
      return {sycl::property::queue::enable_profiling{}, props...};
#endif
      return {props...};
    };

    auto dev_name_selector = [=](const sycl::device& dev){
    	std::string check_name = dev.get_info<sycl::info::device::name>() + " (" + dev.get_platform().get_info<sycl::info::platform::name>() + ")";
    	std::cout << "Checking " << device_type << " against " << check_name << " -> " << (device_type == check_name) << std::endl;
    	return check_name == device_type ? 1 : -1;
    };

    sycl::device dev;

    if(device_type == "cpu") {
      dev = sycl::device(sycl::cpu_selector_v);
    } else if(device_type == "gpu") {
      dev = sycl::device(sycl::gpu_selector_v);
    } else if(device_type == "default") {
      dev = sycl::device(sycl::default_selector_v);
    } else {
      dev = sycl::device(dev_name_selector);
    }
    
    std::cout << "Selected device: " << dev.get_info<sycl::info::device::name>() << " (" << dev.get_platform().get_info<sycl::info::platform::name>() << ")" << std::endl;
    
    return sycl::queue{dev, getQueueProperties()};
  }

  CommandLine cli_parser;
};

#endif
