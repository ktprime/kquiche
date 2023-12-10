#include "quiche/quic/core/crypto/proof_source_x509.h"
#include <utility>

extern std::string cert_file; //"certificate chain."
extern std::string key_file;  //"the pkcs8 private key.");

namespace quiche {

static bool ReadFileToString(const std::string& cfg_name, std::string& server_cfg)
{
    FILE* f = fopen(cfg_name.c_str(), "rb");
    if (!f) {
        server_cfg.clear();
        return false;
    }

    // Determine file size
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    server_cfg.resize(size);
    rewind(f);
    size = fread((char*)server_cfg.c_str(), sizeof(char), size, f);
    if (size != server_cfg.size())
        server_cfg.resize(size);

    fclose(f);
    return server_cfg.size() > 0;
}

std::unique_ptr<quic::ProofSource> CreateDefaultProofSourceImpl() {

//  auto& cert_path = cert_file;
//  auto& key_path  = key_file;

  // Initialize OpenSSL if it isn't already initialized. This must be called
  // before any other OpenSSL functions though it is safe and cheap to call this
  // multiple times.
  // This function is thread-safe, and OpenSSL will only ever be initialized once.
  // OpenSSL will be properly shut down on program exit.
  // CRYPTO_library_init may be safely called concurrently.
  // CRYPTO_library_init();

 auto cert_data = cert_file;

#if 0
  if (!ReadFileToString(cert_path, cert_data)) {
    QUIC_DLOG(FATAL) << "Unable to read certificates." << cert_path;
    return nullptr;
  }
#endif

  std::stringstream cert_stream(cert_data);
  std::vector<std::string> certs = quic::CertificateView::LoadPemFromStream(&cert_stream);

  auto default_chain = quiche::QuicheReferenceCountedPointer<quic::ProofSource::Chain>(new quic::ProofSource::Chain(certs));
  auto key_data = key_file;
#if 0
  if (!ReadFileToString(key_path, key_data)) {
    QUIC_DLOG(FATAL) << "Unable to read key." << key_path;
    return nullptr;
  }
#endif

  std::unique_ptr<quic::CertificatePrivateKey> default_key = quic::CertificatePrivateKey::LoadFromDer(key_data);
  if (default_key == nullptr) {
    QUIC_DLOG(FATAL) << "default key is null.";
    return nullptr;
  }

//  printf("===================== load key success -------------------------------------\n");
  return quic::ProofSourceX509::Create(default_chain, std::move(*default_key));
}

}  // namespace quic
