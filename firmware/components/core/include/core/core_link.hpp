#pragma once
#include <cstddef>
#include <memory>
#include <cstdint>
namespace uart_comm { class UartComm; }
namespace core {
class Core;
class CoreLink {
public:
    CoreLink(Core &core,uart_comm::UartComm &uart);
    ~CoreLink();
    bool isValid() const;
    uint32_t stackHighWater() const;
    // The heap a link takes besides the groups' streams (RAM estimate).
    static size_t ramBytes();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
