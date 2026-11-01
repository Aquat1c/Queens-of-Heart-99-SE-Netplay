#include "TestHarness.h"

#include "net/PortMapper.h"

#include <cstring>
#include <string>

using namespace qnet::portmap;
using namespace qnet::portmap::wire;

QNP_TEST(NatPmpRequestIsTwelveBigEndianBytes) {
    uint8_t out[kNatPmpRequestBytes]{};
    encodeNatPmpUdpRequest(61513u, 61513u, kLeaseSeconds, out);
    QNP_CHECK(out[0] == 0u);
    QNP_CHECK(out[1] == 1u);
    QNP_CHECK(out[2] == 0u && out[3] == 0u);
    QNP_CHECK(out[4] == 0xF0u && out[5] == 0x49u);
    QNP_CHECK(out[6] == 0xF0u && out[7] == 0x49u);
    QNP_CHECK(out[8] == 0u && out[9] == 0u);
    QNP_CHECK(out[10] == 0x0Eu && out[11] == 0x10u);

    encodeNatPmpUdpRequest(61513u, 0u, 0u, out);
    QNP_CHECK(out[6] == 0u && out[7] == 0u);
    QNP_CHECK(out[8] == 0u && out[9] == 0u && out[10] == 0u && out[11] == 0u);
}

QNP_TEST(NatPmpReplyIsRejectedUnlessItAnswersOurRequest) {
    NatPmpReply reply{};
    const uint8_t good[kNatPmpResponseBytes] = {
        0, 129, 0, 0,
        0, 0, 0, 0,
        0xF0, 0x49,
        0xF0, 0x49,
        0, 0, 0x0E, 0x10,
    };
    QNP_REQUIRE(decodeNatPmpReply(good, sizeof(good), reply));
    QNP_CHECK(reply.resultCode == 0u);
    QNP_CHECK(reply.internalPort == 61513u);
    QNP_CHECK(reply.externalPort == 61513u);
    QNP_CHECK(reply.lifetimeSeconds == 3600u);

    QNP_CHECK(!decodeNatPmpReply(good, sizeof(good) - 1, reply));
    uint8_t wrong[kNatPmpResponseBytes];
    std::memcpy(wrong, good, sizeof(wrong));
    wrong[0] = 1u;
    QNP_CHECK(!decodeNatPmpReply(wrong, sizeof(wrong), reply));
    std::memcpy(wrong, good, sizeof(wrong));
    wrong[1] = 130u;
    QNP_CHECK(!decodeNatPmpReply(wrong, sizeof(wrong), reply));
    QNP_CHECK(!decodeNatPmpReply(nullptr, sizeof(good), reply));

    std::memcpy(wrong, good, sizeof(wrong));
    wrong[3] = 2u;
    QNP_REQUIRE(decodeNatPmpReply(wrong, sizeof(wrong), reply));
    QNP_CHECK(reply.resultCode == 2u);
}

QNP_TEST(SsdpSearchNamesTheTargetAndTheMulticastHost) {
    const std::string search = buildSsdpSearch(
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1", 2);
    QNP_CHECK(search.compare(0, 8, "M-SEARCH") == 0);
    QNP_CHECK(search.find("HOST: 239.255.255.250:1900\r\n")
              != std::string::npos);
    QNP_CHECK(search.find("MAN: \"ssdp:discover\"\r\n") != std::string::npos);
    QNP_CHECK(search.find("MX: 2\r\n") != std::string::npos);
    QNP_CHECK(search.find("ST: urn:schemas-upnp-org:device:"
                          "InternetGatewayDevice:1\r\n")
              != std::string::npos);
    QNP_CHECK(search.size() >= 4
              && search.compare(search.size() - 4, 4, "\r\n\r\n") == 0);
    QNP_CHECK(buildSsdpSearch(nullptr, 1).find("ST: ssdp:all")
              != std::string::npos);
}

QNP_TEST(SsdpLocationIsReadCaseInsensitivelyAndOnlyWhenItIsAUrl) {
    const std::string reply =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "Location: http://192.168.0.1:5000/rootDesc.xml\r\n"
        "SERVER: Linux/3.4 UPnP/1.0\r\n\r\n";
    std::string location;
    QNP_REQUIRE(parseSsdpLocation(reply, location));
    QNP_CHECK(location == "http://192.168.0.1:5000/rootDesc.xml");

    std::string lf = reply;
    std::string::size_type at = 0;
    while ((at = lf.find("\r\n", at)) != std::string::npos)
        lf.replace(at, 2, "\n");
    QNP_REQUIRE(parseSsdpLocation(lf, location));
    QNP_CHECK(location == "http://192.168.0.1:5000/rootDesc.xml");

    QNP_CHECK(!parseSsdpLocation(
        "HTTP/1.1 200 OK\r\nLOCATION: https://192.168.0.1/desc.xml\r\n\r\n",
        location));
    QNP_CHECK(!parseSsdpLocation(
        "HTTP/1.1 200 OK\r\nLOCATION: \r\n\r\n", location));
    QNP_CHECK(!parseSsdpLocation("HTTP/1.1 200 OK\r\n\r\n", location));
    QNP_CHECK(!parseSsdpLocation(
        "HTTP/1.1 200 OK\r\nSERVER: my-location: http://evil/\r\n\r\n",
        location));
}

QNP_TEST(HttpUrlsParseWithAndWithoutPortsAndPaths) {
    HttpUrl url{};
    QNP_REQUIRE(parseHttpUrl("http://192.168.0.1:5000/rootDesc.xml", url));
    QNP_CHECK(url.host == "192.168.0.1");
    QNP_CHECK(url.port == 5000u);
    QNP_CHECK(url.path == "/rootDesc.xml");

    QNP_REQUIRE(parseHttpUrl("http://192.168.1.254/desc", url));
    QNP_CHECK(url.port == 80u);
    QNP_CHECK(url.path == "/desc");

    QNP_REQUIRE(parseHttpUrl("http://10.0.0.1", url));
    QNP_CHECK(url.path == "/");

    QNP_CHECK(!parseHttpUrl("https://192.168.0.1/x", url));
    QNP_CHECK(!parseHttpUrl("http://", url));
    QNP_CHECK(!parseHttpUrl("http://host:0/x", url));
    QNP_CHECK(!parseHttpUrl("http://host:99999/x", url));
    QNP_CHECK(!parseHttpUrl("http://host:abc/x", url));
}

QNP_TEST(ControlUrlResolvesButNeverLeavesTheDeviceThatDescribedItself) {
    HttpUrl base{};
    QNP_REQUIRE(parseHttpUrl("http://192.168.0.1:5000/desc/root.xml", base));
    HttpUrl control{};

    QNP_REQUIRE(resolveControlUrl(base, "/ctl/IPConn", control));
    QNP_CHECK(control.host == "192.168.0.1" && control.port == 5000u);
    QNP_CHECK(control.path == "/ctl/IPConn");

    QNP_REQUIRE(resolveControlUrl(base, "ctl/IPConn", control));
    QNP_CHECK(control.path == "/desc/ctl/IPConn");

    QNP_REQUIRE(resolveControlUrl(
        base, "http://192.168.0.1:5000/ctl", control));
    QNP_CHECK(control.path == "/ctl");

    QNP_CHECK(!resolveControlUrl(base, "http://10.9.9.9:5000/ctl", control));
    QNP_CHECK(!resolveControlUrl(base, "http://192.168.0.1:80/ctl", control));
    QNP_CHECK(!resolveControlUrl(base, "", control));
}

QNP_TEST(WanConnectionServiceIsFoundAndPreferredInOrder) {
    const std::string xml =
        "<root><device><serviceList>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1"
        "</serviceType><controlURL>/wrong</controlURL></service>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1"
        "</serviceType><controlURL>/ppp</controlURL></service>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1"
        "</serviceType><controlURL>/ip</controlURL></service>"
        "</serviceList></device></root>";
    IgdService service{};
    QNP_REQUIRE(findWanConnectionService(xml, service));
    QNP_CHECK(service.serviceType
              == "urn:schemas-upnp-org:service:WANIPConnection:1");
    QNP_CHECK(service.controlUrl == "/ip");

    const std::string pppOnly =
        "<root><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1"
        "</serviceType><controlURL>  /ppp  </controlURL></service></root>";
    QNP_REQUIRE(findWanConnectionService(pppOnly, service));
    QNP_CHECK(service.controlUrl == "/ppp");

    QNP_CHECK(!findWanConnectionService(
        "<root><device><friendlyName>printer</friendlyName></device></root>",
        service));
    QNP_CHECK(!findWanConnectionService("", service));
}

QNP_TEST(AddPortMappingBodyCarriesUdpAndALease) {
    AddMapping mapping{};
    mapping.externalPort = 7501u;
    mapping.internalPort = 7501u;
    mapping.internalClient = "192.168.0.6";
    mapping.description = "QOH99 spectators";
    mapping.leaseSeconds = kLeaseSeconds;
    const std::string body = buildAddPortMappingBody(
        "urn:schemas-upnp-org:service:WANIPConnection:1", mapping);
    QNP_CHECK(body.find("<u:AddPortMapping") != std::string::npos);
    QNP_CHECK(body.find("<NewProtocol>UDP</NewProtocol>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewExternalPort>7501</NewExternalPort>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewInternalPort>7501</NewInternalPort>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewInternalClient>192.168.0.6</NewInternalClient>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewLeaseDuration>3600</NewLeaseDuration>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewEnabled>1</NewEnabled>") != std::string::npos);
}

QNP_TEST(SoapBodiesEscapeWhatevertheDescriptionContains) {
    AddMapping mapping{};
    mapping.externalPort = 1u;
    mapping.internalPort = 1u;
    mapping.internalClient = "10.0.0.2";
    mapping.description = "a<b>&\"c\"";
    const std::string body = buildAddPortMappingBody("urn:x", mapping);
    QNP_CHECK(body.find("a&lt;b&gt;&amp;&quot;c&quot;") != std::string::npos);
    QNP_CHECK(body.find("<b>&") == std::string::npos);
    mapping.description = std::string("bad\x01here");
    const std::string stripped = buildAddPortMappingBody("urn:x", mapping);
    QNP_CHECK(stripped.find("badhere") != std::string::npos);
}

QNP_TEST(DeleteBodyAndSoapRequestAreWellFormed) {
    const std::string body = buildDeletePortMappingBody(
        "urn:schemas-upnp-org:service:WANIPConnection:1", 7501u);
    QNP_CHECK(body.find("<u:DeletePortMapping") != std::string::npos);
    QNP_CHECK(body.find("<NewExternalPort>7501</NewExternalPort>")
              != std::string::npos);
    QNP_CHECK(body.find("<NewProtocol>UDP</NewProtocol>")
              != std::string::npos);

    HttpUrl target{};
    QNP_REQUIRE(parseHttpUrl("http://192.168.0.1:5000/ctl/IPConn", target));
    const std::string request = buildSoapRequest(
        target, "urn:schemas-upnp-org:service:WANIPConnection:1",
        "DeletePortMapping", body);
    QNP_CHECK(request.compare(0, 22, "POST /ctl/IPConn HTTP/") == 0);
    QNP_CHECK(request.find("HOST: 192.168.0.1:5000\r\n") != std::string::npos);
    QNP_CHECK(request.find("SOAPACTION: \"urn:schemas-upnp-org:service:"
                           "WANIPConnection:1#DeletePortMapping\"\r\n")
              != std::string::npos);
    QNP_CHECK(request.find("CONTENT-LENGTH: " + std::to_string(body.size())
                           + "\r\n") != std::string::npos);
    std::string sent;
    QNP_REQUIRE(splitHttpBody(request, sent));
    QNP_CHECK(sent == body);
}

QNP_TEST(RouterAnswersAreReadForStatusAndForRefusalReason) {
    QNP_CHECK(parseHttpStatus("HTTP/1.1 200 OK\r\n\r\n") == 200);
    QNP_CHECK(parseHttpStatus("HTTP/1.0 500 Internal Server Error\r\n\r\n")
              == 500);
    QNP_CHECK(parseHttpStatus("not http at all") == 0);
    QNP_CHECK(parseHttpStatus("") == 0);

    const std::string conflict =
        "HTTP/1.1 500 Internal Server Error\r\n\r\n"
        "<s:Envelope><s:Body><s:Fault><detail><UPnPError>"
        "<errorCode>718</errorCode>"
        "<errorDescription>ConflictInMappingEntry</errorDescription>"
        "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    QNP_CHECK(parseHttpStatus(conflict) == 500);
    QNP_CHECK(parseUpnpErrorCode(conflict) == 718);
    QNP_CHECK(parseUpnpErrorCode("HTTP/1.1 200 OK\r\n\r\n<ok/>") == 0);
    QNP_CHECK(parseUpnpErrorCode("<errorCode>7x8</errorCode>") == 0);
}

QNP_TEST(HttpBodySplitFailsClosedWithoutATerminator) {
    std::string body;
    QNP_CHECK(splitHttpBody("HTTP/1.1 200 OK\r\n\r\nhello", body));
    QNP_CHECK(body == "hello");
    QNP_CHECK(splitHttpBody("HTTP/1.1 200 OK\n\nhello", body));
    QNP_CHECK(body == "hello");
    QNP_CHECK(!splitHttpBody("HTTP/1.1 200 OK\r\nno terminator", body));
    QNP_CHECK(body.empty());
}
