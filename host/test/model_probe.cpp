#include "drivelink/model.hpp"
#include <iostream>

// Read-only public-API consumer, also usable on the bench board. The TSV is a
// test protocol, not a new CLI output format. No module-specific discovery.
int main(int argc, char **argv)
{
    if (argc != 2) { return 2; }
    try
    {
        drivelink::Session session(argv[1]);
        drivelink::Client client(session);
        drivelink::DeviceModel model(client);
        model.reload();
        const auto &snapshot = model.snapshot();
        std::cout << "INFO\t" << unsigned(snapshot.info.flags) << '\t' << snapshot.info.build_id << '\n';
        for (const auto &[type_id, type] : snapshot.types)
        {
            std::cout << "TYPE\t" << unsigned(type_id) << '\t' << type.descriptor.name << '\n';
            for (const auto &[item_id, item] : type.items)
            {
                for (const auto &e : item.enums)
                {
                    std::cout << "ENUM\t" << unsigned(type_id) << '\t' << item_id << '\t'
                              << unsigned(e.value) << '\t' << e.name << '\n';
                }
            }
        }
        for (const auto &[id, value] : snapshot.system_params)
        {
            std::cout << "SYSTEM\t" << id << '\t' << drivelink::valueBits(value) << '\n';
        }
        for (auto pair : {std::pair{"CONFIG", &snapshot.configured}, std::pair{"PENDING", &snapshot.pending}})
        {
            for (const auto &[key, entry] : *pair.second)
            {
                std::cout << "INSTANCE\t" << pair.first << '\t' << unsigned(key.first) << '\t'
                          << unsigned(key.second) << '\t' << unsigned(entry.instance.loop) << '\t'
                          << unsigned(entry.instance.state) << '\t' << entry.instance.label << '\n';
                for (const auto &[id, value] : entry.params)
                {
                    std::cout << "PARAM\t" << pair.first << '\t' << unsigned(key.first) << '\t'
                              << unsigned(key.second) << '\t' << id << '\t' << drivelink::valueBits(value) << '\n';
                }
            }
        }
        std::cout << "CODES\t" << snapshot.codes.size() << '\n';
    }
    catch (const drivelink::Error &e)
    {
        std::cerr << e.what() << '\n';
        return static_cast<int>(e.kind);
    }
}
