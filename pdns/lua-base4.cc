#include "config.h"
#include <cassert>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <typeinfo>
#include <sys/stat.h>
#include "logger.hh"
#include "logging.hh"
#include "iputils.hh"
#include "dnsname.hh"
#include "dnsparser.hh"
#include "dnspacket.hh"
#include "namespaces.hh"
#include "ednssubnet.hh"
#include "lua-base4.hh"
#include "ext/luawrapper/include/LuaContext.hpp"
#include "dns_random.hh"

void BaseLua4::loadFile(const std::string& fname, bool doPostLoad)
{
  std::ifstream ifs(fname);
  if (!ifs) {
    auto ret = errno;
    auto msg = stringerror(ret);
    g_log << Logger::Error << "Unable to read configuration file from '" << fname << "': " << msg << endl;
    throw std::runtime_error(msg);
  }
  loadStream(ifs, doPostLoad);
};

void BaseLua4::loadString(const std::string &script) {
  std::istringstream iss(script);
  loadStream(iss, true);
};

void BaseLua4::includePath(const std::string& directory) {
  std::vector<std::string> vec;
  const std::string& suffix = "lua";
  auto directoryError = pdns::visit_directory(directory, [this, &directory, &suffix, &vec]([[maybe_unused]] ino_t inodeNumber, const std::string_view& name) {
    (void)this;
    if (boost::starts_with(name, ".")) {
      return true; // skip any dots
    }
    if (boost::ends_with(name, suffix)) {
      // build name
      string fullName = directory + "/" + std::string(name);
      // ensure it's readable file
      struct stat statInfo
      {
      };
      if (stat(fullName.c_str(), &statInfo) != 0 || !S_ISREG(statInfo.st_mode)) {
        string msg = fullName + " is not a regular file";
        g_log << Logger::Error << msg << std::endl;
        throw PDNSException(std::move(msg));
      }
      vec.emplace_back(fullName);
    }
    return true;
  });

  if (directoryError) {
    int err = errno;
    string msg = directory + " is not accessible: " + stringerror(err);
    g_log << Logger::Error << msg << std::endl;
    throw PDNSException(std::move(msg));
  }

  std::sort(vec.begin(), vec.end(), CIStringComparePOSIX());

  for(const auto& file: vec) {
    loadFile(file, false);
  }
};

//  By default no features
void BaseLua4::getFeatures(Features &) { }

void BaseLua4::prepareContext() {
  d_lw = std::make_unique<LuaContext>();

  // lua features available
  Features features;
  getFeatures(features);
  d_lw->writeVariable("pdns_features", features);

  // dnsheader
  d_lw->registerFunction<int(dnsheader::*)()>("getID", [](dnsheader& dh) { return ntohs(dh.id); });
  d_lw->registerFunction<bool(dnsheader::*)()>("getCD", [](dnsheader& dh) { return dh.cd; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getTC", [](dnsheader& dh) { return dh.tc; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getRA", [](dnsheader& dh) { return dh.ra; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getAD", [](dnsheader& dh) { return dh.ad; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getAA", [](dnsheader& dh) { return dh.aa; });
  d_lw->registerFunction<bool(dnsheader::*)()>("getRD", [](dnsheader& dh) { return dh.rd; });
  d_lw->registerFunction<int(dnsheader::*)()>("getRCODE", [](dnsheader& dh) { return dh.rcode; });
  d_lw->registerFunction<int(dnsheader::*)()>("getOPCODE", [](dnsheader& dh) { return dh.opcode; });
  d_lw->registerFunction<int(dnsheader::*)()>("getQDCOUNT", [](dnsheader& dh) { return ntohs(dh.qdcount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getANCOUNT", [](dnsheader& dh) { return ntohs(dh.ancount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getNSCOUNT", [](dnsheader& dh) { return ntohs(dh.nscount); });
  d_lw->registerFunction<int(dnsheader::*)()>("getARCOUNT", [](dnsheader& dh) { return ntohs(dh.arcount); });

  // DNSName
  d_lw->writeFunction("newDN", [](const std::string& dom){ return DNSName(dom); });
  d_lw->registerFunction("__lt", &DNSName::operator<);
  d_lw->registerFunction("canonCompare", &DNSName::canonCompare);
  d_lw->registerFunction<DNSName(DNSName::*)(const DNSName&)>("makeRelative", [](const DNSName& name, const DNSName& zone) { return name.makeRelative(zone); });
  d_lw->registerFunction<bool(DNSName::*)(const DNSName&)>("isPartOf", [](const DNSName& name, const DNSName& rhs) { return name.isPartOf(rhs); });
  d_lw->registerFunction("getRawLabels", &DNSName::getRawLabels);
  d_lw->registerFunction<unsigned int(DNSName::*)()>("countLabels", [](const DNSName& name) { return name.countLabels(); });
  d_lw->registerFunction<size_t(DNSName::*)()>("wireLength", [](const DNSName& name) { return name.wirelength(); });
  d_lw->registerFunction<size_t(DNSName::*)()>("wirelength", [](const DNSName& name) { return name.wirelength(); });
  d_lw->registerFunction<bool(DNSName::*)(const std::string&)>("equal", [](const DNSName& lhs, const std::string& rhs) { return lhs==DNSName(rhs); });
  d_lw->registerEqFunction(&DNSName::operator==);
  d_lw->registerToStringFunction<string(DNSName::*)()>([](const DNSName&dn ) { return dn.toString(); });
  d_lw->registerFunction<string(DNSName::*)()>("toString", [](const DNSName&dn ) { return dn.toString(); });
  d_lw->registerFunction<string(DNSName::*)()>("toStringNoDot", [](const DNSName&dn ) { return dn.toStringNoDot(); });
  d_lw->registerFunction<bool(DNSName::*)()>("chopOff", [](DNSName&dn ) { return dn.chopOff(); });

  // DNSResourceRecord
  d_lw->writeFunction("newDRR", [](const DNSName& qname, const string& qtype, const unsigned int ttl, const string& content, boost::optional<int> domain_id, boost::optional<int> auth){
    auto drr = DNSResourceRecord();
    drr.qname = qname;
    drr.qtype = qtype;
    drr.ttl = ttl;
    drr.setContent(content);
    if (domain_id)
      drr.domain_id = *domain_id;
    if (auth)
      drr.auth = *auth;
     return drr;
  });
  d_lw->registerEqFunction(&DNSResourceRecord::operator==);
  d_lw->registerFunction("__lt", &DNSResourceRecord::operator<);
  d_lw->registerToStringFunction<string(DNSResourceRecord::*)()>([](const DNSResourceRecord& rec) { return rec.getZoneRepresentation(); });
  d_lw->registerFunction<string(DNSResourceRecord::*)()>("toString", [](const DNSResourceRecord& rec) { return rec.getZoneRepresentation();} );
  d_lw->registerFunction<DNSName(DNSResourceRecord::*)()>("qname", [](DNSResourceRecord& rec) { return rec.qname; });
  d_lw->registerFunction<DNSName(DNSResourceRecord::*)()>("wildcardName", [](DNSResourceRecord& rec) { return rec.wildcardname; });
  d_lw->registerFunction<string(DNSResourceRecord::*)()>("content", [](DNSResourceRecord& rec) { return rec.content; });
  d_lw->registerFunction<time_t(DNSResourceRecord::*)()>("lastModified", [](DNSResourceRecord& rec) { return rec.last_modified; });
  d_lw->registerFunction<uint32_t(DNSResourceRecord::*)()>("ttl", [](DNSResourceRecord& rec) { return rec.ttl; });
  d_lw->registerFunction<uint32_t(DNSResourceRecord::*)()>("signttl", [](DNSResourceRecord& rec) { return rec.signttl; });
  d_lw->registerFunction<int(DNSResourceRecord::*)()>("domainId", [](DNSResourceRecord& rec) { return rec.domain_id; });
  d_lw->registerFunction<uint16_t(DNSResourceRecord::*)()>("qtype", [](DNSResourceRecord& rec) { return rec.qtype.getCode(); });
  d_lw->registerFunction<uint16_t(DNSResourceRecord::*)()>("qclass", [](DNSResourceRecord& rec) { return rec.qclass; });
  d_lw->registerFunction<uint8_t(DNSResourceRecord::*)()>("scopeMask", [](DNSResourceRecord& rec) { return rec.scopeMask; });
  d_lw->registerFunction<bool(DNSResourceRecord::*)()>("auth", [](DNSResourceRecord& rec) { return rec.auth; });
  d_lw->registerFunction<bool(DNSResourceRecord::*)()>("disabled", [](DNSResourceRecord& rec) { return rec.disabled; });

  // ComboAddress
  d_lw->registerFunction<bool(ComboAddress::*)()>("isIPv4", [](const ComboAddress& addr) { return addr.sin4.sin_family == AF_INET; });
  d_lw->registerFunction<bool(ComboAddress::*)()>("isIPv6", [](const ComboAddress& addr) { return addr.sin4.sin_family == AF_INET6; });
  d_lw->registerFunction<uint16_t(ComboAddress::*)()>("getPort", [](const ComboAddress& addr) { return ntohs(addr.sin4.sin_port); } );
  d_lw->registerFunction<bool(ComboAddress::*)()>("isMappedIPv4", [](const ComboAddress& addr) { return addr.isMappedIPv4(); });
  d_lw->registerFunction<ComboAddress(ComboAddress::*)()>("mapToIPv4", [](const ComboAddress& addr) { return addr.mapToIPv4(); });
  d_lw->registerFunction<void(ComboAddress::*)(unsigned int)>("truncate", [](ComboAddress& addr, unsigned int bits) { addr.truncate(bits); });
  d_lw->registerFunction<string(ComboAddress::*)()>("toString", [](const ComboAddress& addr) { return addr.toString(); });
  d_lw->registerToStringFunction<string(ComboAddress::*)()>([](const ComboAddress& addr) { return addr.toString(); });
  d_lw->registerFunction<string(ComboAddress::*)()>("toStringWithPort", [](const ComboAddress& addr) { return addr.toStringWithPort(); });
  d_lw->registerFunction<string(ComboAddress::*)()>("getRaw", [](const ComboAddress& addr) { return addr.toByteString(); });

  d_lw->writeFunction("newCA", [](const std::string& a) { return ComboAddress(a); });
  d_lw->writeFunction("newCAFromRaw", [](const std::string& raw, boost::optional<uint16_t> port) {
                                        if (raw.size() == 4) {
                                          struct sockaddr_in sin4;
                                          memset(&sin4, 0, sizeof(sin4));
                                          sin4.sin_family = AF_INET;
                                          memcpy(&sin4.sin_addr.s_addr, raw.c_str(), raw.size());
                                          if (port) {
                                            sin4.sin_port = htons(*port);
                                          }
                                          return ComboAddress(&sin4);
                                        }
                                        else if (raw.size() == 16) {
                                          struct sockaddr_in6 sin6;
                                          memset(&sin6, 0, sizeof(sin6));
                                          sin6.sin6_family = AF_INET6;
                                          memcpy(&sin6.sin6_addr.s6_addr, raw.c_str(), raw.size());
                                          if (port) {
                                            sin6.sin6_port = htons(*port);
                                          }
                                          return ComboAddress(&sin6);
                                        }
                                        return ComboAddress();
                                      });
  typedef std::unordered_set<ComboAddress,ComboAddress::addressOnlyHash,ComboAddress::addressOnlyEqual> cas_t;
  d_lw->registerFunction<bool(ComboAddress::*)(const ComboAddress&)>("equal", [](const ComboAddress& lhs, const ComboAddress& rhs) { return ComboAddress::addressOnlyEqual()(lhs, rhs); });

  // cas_t
  d_lw->writeFunction("newCAS", []{ return cas_t(); });
  d_lw->registerFunction<void(cas_t::*)(boost::variant<string,ComboAddress, vector<pair<unsigned int,string> > >)>("add",
    [](cas_t& cas, const boost::variant<string,ComboAddress,vector<pair<unsigned int,string> > >& in)
    {
      try {
      if(auto s = boost::get<string>(&in)) {
        cas.insert(ComboAddress(*s));
      }
      else if(auto v = boost::get<vector<pair<unsigned int, string> > >(&in)) {
        for(const auto& str : *v)
          cas.insert(ComboAddress(str.second));
      }
      else
        cas.insert(boost::get<ComboAddress>(in));
      }
      catch(std::exception& e) {
        SLOG(g_log <<Logger::Error<<e.what()<<endl,
             g_slog->withName("lua")->error(Logr::Error, e.what(), "Exception in newCAS", "exception", Logging::Loggable("std::exception")));
      }
    });
  d_lw->registerFunction<bool(cas_t::*)(const ComboAddress&)>("check",[](const cas_t& cas, const ComboAddress&ca) { return cas.count(ca)>0; });

  // QType
  d_lw->writeFunction("newQType", [](const string& s) { QType q; q = s; return q; });
  d_lw->registerFunction("getCode", &QType::getCode);
  d_lw->registerFunction("getName", &QType::toString);
  d_lw->registerEqFunction<bool(QType::*)(const QType&)>([](const QType& a, const QType& b){ return a == b;}); // operator overloading confuses LuaContext
  d_lw->registerToStringFunction(&QType::toString);

  // Netmask
  d_lw->writeFunction("newNetmask", [](const string& s) { return Netmask(s); });
  d_lw->registerFunction<ComboAddress(Netmask::*)()>("getNetwork", [](const Netmask& nm) { return nm.getNetwork(); } ); // const reference makes this necessary
  d_lw->registerFunction<ComboAddress(Netmask::*)()>("getMaskedNetwork", [](const Netmask& nm) { return nm.getMaskedNetwork(); } );
  d_lw->registerFunction("isIpv4", &Netmask::isIPv4);
  d_lw->registerFunction("isIPv4", &Netmask::isIPv4);
  d_lw->registerFunction("isIpv6", &Netmask::isIPv6);
  d_lw->registerFunction("isIPv6", &Netmask::isIPv6);
  d_lw->registerFunction("getBits", &Netmask::getBits);
  d_lw->registerFunction("toString", &Netmask::toString);
  d_lw->registerFunction("empty", &Netmask::empty);
  d_lw->registerFunction("match", (bool (Netmask::*)(const string&) const)&Netmask::match);
  d_lw->registerEqFunction(&Netmask::operator==);
  d_lw->registerToStringFunction(&Netmask::toString);

  // NetmaskGroup
  d_lw->writeFunction("newNMG", [](boost::optional<vector<pair<unsigned int, std::string>>> masks) {
    auto nmg = NetmaskGroup();

    if (masks) {
      for(const auto& mask: *masks) {
        nmg.addMask(mask.second);
      }
    }

    return nmg;
  });
  // d_lw->writeFunction("newNMG", []() { return NetmaskGroup(); });
  d_lw->registerFunction<void(NetmaskGroup::*)(const std::string&mask)>("addMask", [](NetmaskGroup&nmg, const std::string& mask) { nmg.addMask(mask); });
  d_lw->registerFunction<void(NetmaskGroup::*)(const vector<pair<unsigned int, std::string>>&)>("addMasks", [](NetmaskGroup&nmg, const vector<pair<unsigned int, std::string>>& masks) { for(const auto& mask: masks) { nmg.addMask(mask.second); } });
  d_lw->registerFunction("match", (bool (NetmaskGroup::*)(const ComboAddress&) const)&NetmaskGroup::match);

  // DNSRecord
  d_lw->writeFunction("newDR", [](const DNSName& name, const std::string& type, unsigned int ttl, const std::string& content, int place) { QType qtype; qtype = type; auto dr = DNSRecord(); dr.d_name = name; dr.d_type = qtype.getCode(); dr.d_ttl = ttl; dr.setContent(shared_ptr<DNSRecordContent>(DNSRecordContent::make(dr.d_type, QClass::IN, content))); dr.d_place = static_cast<DNSResourceRecord::Place>(place); return dr; });
  d_lw->registerMember("name", &DNSRecord::d_name);
  d_lw->registerMember("type", &DNSRecord::d_type);
  d_lw->registerMember("ttl", &DNSRecord::d_ttl);
  d_lw->registerMember("place", &DNSRecord::d_place);
  d_lw->registerFunction<string(DNSRecord::*)()>("getContent", [](const DNSRecord& dr) { return dr.getContent()->getZoneRepresentation(); });
  d_lw->registerFunction<boost::optional<ComboAddress>(DNSRecord::*)()>("getCA", [](const DNSRecord& dr) {
      boost::optional<ComboAddress> ret;

      if(auto arec = getRR<ARecordContent>(dr))
        ret=arec->getCA(53);
      else if(auto aaaarec = getRR<AAAARecordContent>(dr))
        ret=aaaarec->getCA(53);
      return ret;
    });
  d_lw->registerFunction<void (DNSRecord::*)(const std::string&)>("changeContent", [](DNSRecord& dr, const std::string& newContent) { dr.setContent(shared_ptr<DNSRecordContent>(DNSRecordContent::make(dr.d_type, 1, newContent))); });

  // pdnslog
#ifdef RECURSOR
  d_lw->writeFunction("pdnslog", [](const std::string& msg, boost::optional<int> loglevel, boost::optional<std::map<std::string, std::string>> values) {
    auto log = g_slog->withName("lua");
    if (values) {
      for (const auto& [key, value] : *values) {
        log = log->withValues(key, Logging::Loggable(value));
      }
    }
    log->info(static_cast<Logr::Priority>(loglevel.get_value_or(Logr::Warning)), msg);
#else
    d_lw->writeFunction("pdnslog", [](const std::string& msg, boost::optional<int> loglevel) {
      g_log << (Logger::Urgency)loglevel.get_value_or(Logger::Warning) << msg<<endl;
#endif
  });

  d_lw->writeFunction("pdnsrandom", [](boost::optional<uint32_t> maximum) {
    return maximum ? dns_random(*maximum) : dns_random_uint32();
  });

  // certain constants

  vector<pair<string, int> > rcodes = {{"NOERROR",  RCode::NoError  },
                                       {"FORMERR",  RCode::FormErr  },
                                       {"SERVFAIL", RCode::ServFail },
                                       {"NXDOMAIN", RCode::NXDomain },
                                       {"NOTIMP",   RCode::NotImp   },
                                       {"REFUSED",  RCode::Refused  },
                                       {"YXDOMAIN", RCode::YXDomain },
                                       {"YXRRSET",  RCode::YXRRSet  },
                                       {"NXRRSET",  RCode::NXRRSet  },
                                       {"NOTAUTH",  RCode::NotAuth  },
                                       {"NOTZONE",  RCode::NotZone  },
                                       {"DROP",    -2               }}; // To give backport-incompatibility warning
  for(const auto& rcode : rcodes)
    d_pd.push_back({rcode.first, rcode.second});

  d_pd.push_back({"place", in_t{
    {"QUESTION", 0},
    {"ANSWER", 1},
    {"AUTHORITY", 2},
    {"ADDITIONAL", 3}
  }});

  d_pd.push_back({"loglevels", in_t{
        {"Alert", LOG_ALERT},
        {"Critical", LOG_CRIT},
        {"Debug", LOG_DEBUG},
        {"Emergency", LOG_EMERG},
        {"Info", LOG_INFO},
        {"Notice", LOG_NOTICE},
        {"Warning", LOG_WARNING},
        {"Error", LOG_ERR}
          }});

  for(const auto& n : QType::names)
    d_pd.push_back({n.first, n.second});

  d_lw->registerMember("tv_sec", &timeval::tv_sec);
  d_lw->registerMember("tv_usec", &timeval::tv_usec);

  postPrepareContext();

  // so we can let postprepare do changes to this
  d_lw->writeVariable("pdns", d_pd);
}

void BaseLua4::loadStream(std::istream &stream, bool doPostLoad) {
  d_lw->executeCode(stream);

  if (doPostLoad) {
    postLoad();
  }
}

BaseLua4::~BaseLua4() = default;
