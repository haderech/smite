#include <eo/go.h>
#include <iostream>

using namespace eo;

extern func<> eo_main();

int main(int argc, char** argv) {
  try {
    boost::asio::co_spawn(executor, eo_main(), boost::asio::detached);
    executor.join();
  } catch (boost::exception& e) {
    std::cerr << boost::diagnostic_information(e) << std::endl;
  } catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
  } catch (...) {
  }
}