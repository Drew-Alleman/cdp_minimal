#include <iostream>
#include <thread>
#include <chrono>
#include "cdp/browser.h"

int main() {
    cdp::Browser browser("127.0.0.1", 9222);
    if (!browser.connect()) {
        std::cerr << "Failed to connect\n";
        return 1;
    }

    std::cout << "Starting mouse + input tracker...\n";
    browser.watchInput();

    std::cout << ">>> Move mouse and type in fields <<<\n";
    std::this_thread::sleep_for(std::chrono::seconds(30));

    browser.stopInput();
    return 0;
}
