#include "nix/util/serialise.hh"
#include "nix/store/common-protocol.hh"
#include "nix/store/common-protocol-impl.hh"
#include "nix/store/store-dir-config.hh"


namespace nix {

/* protocol-agnostic definitions */

StorePath CommonProto::Serialise<StorePath>::read(const StoreDirConfig & store, CommonProto::ReadConn conn)
{
    return store.parseStorePath(readString(conn.from));
}

void CommonProto::Serialise<StorePath>::write(
    const StoreDirConfig & store, CommonProto::WriteConn conn, const StorePath & storePath)
{
    conn.to << store.printStorePath(storePath);
}

} // namespace nix
