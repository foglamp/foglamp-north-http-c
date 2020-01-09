/*
 * FogLAMP HTTP north plugin.
 *
 * Copyright (c) 2018 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Amandeep Singh Arora, Massimiliano Pinto
 */
#include <iostream>
#include <string>
#include <logger.h>
#include "plugin_api.h"
#include "simple_https.h"
#include "simple_http.h"
#include "reading.h"
#include "config_category.h"
#include "version.h"

using namespace std;

#define PLUGIN_NAME "httpc"

/**
 * Plugin specific default configuration
 */
#define PLUGIN_DEFAULT_CONFIG "\"URL\": { " \
				"\"description\": \"The URL of the HTTP Connector to send data to\", " \
				"\"type\": \"string\", " \
				"\"default\": \"http://localhost:6683/sensor-reading\", \"order\": \"1\", \"displayName\": \"URL\" }, " \
			"\"source\": {" \
				"\"description\": \"Defines the source of the data to be sent on the stream, " \
				"this may be one of either readings, statistics or audit.\", \"type\": \"enumeration\", " \
				"\"default\": \"readings\", "\
				"\"options\": [\"readings\", \"statistics\"], \"order\": \"2\", \"displayName\": \"Source\"}, " \
			"\"retrySleepTime\": { " \
        			"\"description\": \"Seconds between each retry for the communication, " \
                       		"NOTE : the time is doubled at each attempt.\", \"type\": \"integer\", \"default\": \"1\", " \
				"\"order\": \"9\", \"displayName\" : \"Sleep Time Retry\" }, " \
			"\"maxRetry\": { " \
				"\"description\": \"Max number of retries for the communication\", " \
				"\"type\": \"integer\", \"default\": \"3\", " \
				"\"order\": \"10\", \"displayName\" : \"Maximum Retry\" }, " \
			"\"HttpTimeout\": { " \
				"\"description\": \"Timeout in seconds for the HTTP operations with the HTTP Connector Relay\", " \
				"\"type\": \"integer\", \"default\": \"10\", \"order\": \"13\", \"displayName\": \"Http Timeout (in seconds)\"}, " \
			"\"verifySSL\": { " \
        			"\"description\": \"Verify SSL certificate\", " \
				"\"type\": \"boolean\", \"default\": \"False\", \"order\": \"14\", \"displayName\": \"Verify SSL\"}, " \
			"\"applyFilter\": { " \
        			"\"description\": \"Whether to apply filter before processing the data\", " \
				"\"type\": \"boolean\", \"default\": \"False\", \"order\": \"15\", \"displayName\": \"Apply Filter\"}, " \
			"\"filterRule\": { " \
				"\"description\": \"JQ formatted filter to apply (applicable if applyFilter is True)\", " \
				"\"type\": \"string\", \"default\": \".[]\", \"order\": \"16\", \"displayName\": \"Filter Rule\"}"

#define HTTP_NORTH_PLUGIN_DESC "\"plugin\": {\"description\": \"HTTP North C Plugin\", " \
				"\"type\": \"string\", \"default\": \"" PLUGIN_NAME "\", \"readonly\": \"true\"}"

#define PLUGIN_DEFAULT_CONFIG_INFO "{" HTTP_NORTH_PLUGIN_DESC ", " PLUGIN_DEFAULT_CONFIG "}"

/**
 * The HTTP north plugin interface
 */
extern "C" {

/**
 * The C API plugin information structure
 */
static PLUGIN_INFORMATION info = {
	PLUGIN_NAME,			// Name
	VERSION,			// Version
	0,				// Flags
	PLUGIN_TYPE_NORTH,		// Type
	"1.0.0",			// Interface version
	PLUGIN_DEFAULT_CONFIG_INFO	// Configuration
};

/**
 * HTTP Server connector info
 */
typedef struct
{
	string		proto;
	HttpSender	*sender;        // HttpSender is the base class for SimpleHttp and SimpleHttp classes
				        // and sendRequest is a virtual method of HttpSender class
	string		path;
} CONNECTOR_INFO;

uint32_t sendToServer(const vector<Reading *>& readings, CONNECTOR_INFO *connInfo);
const vector<pair<string, string>> createMessageHeader();
const string& getReadingString(const Reading& reading);

/**
 * Return the information about this plugin
 */
PLUGIN_INFORMATION *plugin_info()
{
	return &info;
}

/**
 * Initialise the plugin with configuration.
 *
 * This function is called to get the plugin handle.
 */
PLUGIN_HANDLE plugin_init(ConfigCategory* configData)
{
	Logger::getLogger()->info("http-north C plugin: %s", __FUNCTION__);

	/**
	 * Handle the HTTP(S) parameters here
	 */
	string url = configData->getValue("URL");
	unsigned int retrySleepTime = atoi(configData->getValue("retrySleepTime").c_str());
	unsigned int maxRetry = atoi(configData->getValue("maxRetry").c_str());
	unsigned int timeout = atoi(configData->getValue("HttpTimeout").c_str());

	/**
	 * Extract host, port, path from URL
	 */
	size_t findProtocol = url.find_first_of(":");
	string protocol = url.substr(0,findProtocol);

	string tmpUrl = url.substr(findProtocol + 3);
	size_t findPort = tmpUrl.find_first_of(":");
	string hostName = tmpUrl.substr(0, findPort);

	size_t findPath = tmpUrl.find_first_of("/");
	string port = tmpUrl.substr(findPort + 1 , findPath - findPort -1);
	string path = tmpUrl.substr(findPath);

	/**
	 * Allocate the HTTP(S) handler for "Hostname : port",
	 * connect_timeout and request_timeout.
	 */
	string hostAndPort(hostName + ":" + port);	

	CONNECTOR_INFO *connector_info = new CONNECTOR_INFO;
	if (protocol == string("http"))
		connector_info->sender = new SimpleHttp(hostAndPort,
							timeout,
							timeout,
							retrySleepTime,
							maxRetry);
	else if (protocol == string("https"))
		connector_info->sender = new SimpleHttps(hostAndPort,
							 timeout,
							 timeout,
							 retrySleepTime,
							 maxRetry);
	else
	{
		Logger::getLogger()->error("Didn't find http/https prefix in URL='%s', cannot proceed", url.c_str());
		throw new exception();
	}

	connector_info->path = path;
	connector_info->proto = protocol;
	Logger::getLogger()->info("HTTP plugin configured: URL='%s' ", url.c_str());

	return (PLUGIN_HANDLE) (connector_info);
}

/**
 * Send Readings data to historian server
 */
uint32_t plugin_send(const PLUGIN_HANDLE handle,
		     const vector<Reading *> readings)
{
	CONNECTOR_INFO *connInfo = (CONNECTOR_INFO *) handle;
	return sendToServer(readings, connInfo);
}

/**
 * Shutdown the plugin
 *
 * Delete allocated data
 *
 * @param handle    The plugin handle
 */
void plugin_shutdown(PLUGIN_HANDLE handle)
{
	Logger::getLogger()->info("http-north C plugin: %s", __FUNCTION__);
	CONNECTOR_INFO *connInfo = (CONNECTOR_INFO *) handle;
	delete connInfo->sender;
	delete connInfo;
}

const string& getReadingString(const Reading& reading)
{
	string *value = new string;
	string &m_value = *value;

	// Convert reading data into JSON string
	m_value.append("{\"timestamp\" : \"" + reading.getAssetDateUserTime(Reading::FMT_STANDARD) + "Z" + "\"");
	m_value.append(",\"asset\" : \"" + reading.getAssetName() + "\"");
	m_value.append(",\"readings\" : {");

	// Get reading data
	const vector<Datapoint*> data = reading.getReadingData();

	for (vector<Datapoint*>::const_iterator it = data.begin(); it != data.end(); ++it)
	{
		m_value.append("\"" + (*it)->getName() + "\": " + (*it)->getData().toString() + ",");
	}

	// remove last unrequired comma character because of above loop, if one was added
	auto lastChar = m_value.rbegin();
	if (*lastChar == ',')
		m_value.pop_back();

	m_value.append("}}");
	return m_value;
}

const vector<pair<string, string>> createMessageHeader()
{
	vector<pair<string, string>> res;
	res.push_back(pair<string, string>("Content-Type", "application/json"));
	return res; 
}

uint32_t sendToServer(const vector<Reading *>& readings, CONNECTOR_INFO *connInfo)
{
	ostringstream jsonData;
	jsonData << "[";

	// Fetch Reading* data
	for (vector<Reading *>::const_iterator elem = readings.begin(); elem != readings.end(); ++elem)
	{
		jsonData << getReadingString(**elem) << (elem < (readings.end() -1 ) ? ", " : "");
	}

	jsonData << "]";

	// Create header for Readings data
	vector<pair<string, string>> readingData = createMessageHeader();

	// Build a HTTP(S) POST with readingData headers and 'allReadings' JSON payload
	// Then get HTTPS POST ret code and return 0 to client on error
	try
	{
		int res = connInfo->sender->sendRequest("POST", connInfo->path, readingData, jsonData.str());
		if (res != 200 && res != 204 && res != 201)
		{
			Logger::getLogger()->error("http-north C plugin: Sending JSON readings HTTP(S) error: %d", res);
			return 0;
		}

		Logger::getLogger()->info("http-north C plugin: Successfully sent %d readings", readings.size());

		// Return number of sent readings to the caller
		return readings.size();
	}
	catch (const std::exception& e)
	{
		Logger::getLogger()->error("http-north C plugin: Sending JSON data exception: %s", e.what());
		return 0;
	}
}

// End of extern "C"
};

