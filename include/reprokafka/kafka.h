#ifndef _MOL_DEF_GUARD_DEFINE_MOD_HTTP_REDIS_DEF_GUARD_
#define _MOL_DEF_GUARD_DEFINE_MOD_HTTP_REDIS_DEF_GUARD_

#include <string>
#include <iostream>
#include <sstream>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <map>

#include "priocpp/common.h"
#include "priocpp/api.h"

#include "librdkafka/rdkafka.h"

//////////////////////////////////////////////////////////////


namespace reprokafka   {

class KafkaMsg 
{
public:
	std::string topic;
	std::string msg;
	std::string key;
};

class KafkaConfig
{
public:
	KafkaConfig();
	KafkaConfig(const std::string& brokers);
	~KafkaConfig();
	
	rd_kafka_conf_t* handle();

	void prop(const std::string& key, const std::string& val);

	std::string brokers();

private:
	std::string brokers_;
	std::vector<std::pair<std::string,std::string>> props_;
};


class KafkaTopicConfig
{
public:
	
	rd_kafka_topic_conf_t* handle();

	void prop(const std::string& key, const std::string& val);

private:
	std::vector<std::pair<std::string,std::string>> props_;
};


class KafkaTopic
{
public:

	KafkaTopic(const std::string& name,rd_kafka_topic_t *rk);
	~KafkaTopic();

	std::string name();

	rd_kafka_topic_t* handle();

private:
	std::string name_;
	std::shared_ptr<rd_kafka_topic_t> rkt_producer_ = nullptr;
};


class KafkaSubscription
{
public:

	KafkaSubscription(const std::string& name)
		: topic(name), p(repro::promise<KafkaMsg>())
	{}	

	std::string topic;
	repro::Promise<KafkaMsg> p;
};

class ACK 
{
public:

	ACK()
		: p(repro::promise<>())
	{}

	repro::Promise<> p;
};

class Kafka
{
public:
	Kafka(KafkaConfig& conf);
	~Kafka();

	void connect();
	void consume();
	void stop();

	repro::Future<KafkaMsg> subscribe(const std::string& topic);

	void create_topic(const std::string& topic);
	void create_topic(const std::string& topic, KafkaTopicConfig& topic_conf );

	repro::Future<> send(const std::string& topic,const std::string& msg, int partition = -1);
	repro::Future<> send(const std::string& topic,const std::string& msg, const std::string& key, int partition = -1);

private:

	void msg_consume (rd_kafka_message_t *rkmessage);

	void poll();

	rd_kafka_t *rk_producer_ = nullptr;
	rd_kafka_t *rk_consumer_ = nullptr;
	rd_kafka_topic_partition_list_t* topics_list_ = nullptr;

	std::atomic<bool> stop_;
	std::map<std::string,KafkaSubscription> subscriptions_;
	std::map<std::string,KafkaTopic> topics_;
	std::thread worker_;
};



} // close namespaces

#endif

