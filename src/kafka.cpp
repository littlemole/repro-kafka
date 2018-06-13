#include "reprokafka/kafka.h"

using namespace prio;

namespace reprokafka {



static void rebalance_cb (rd_kafka_t *rk,
                          rd_kafka_resp_err_t err,
			  			  rd_kafka_topic_partition_list_t *partitions,
                          void *opaque) 
{
#ifdef MOL_PROMISE_DEBUG
	fprintf(stderr, "%% Consumer group rebalanced: %i\r\n", err);
#endif
	switch (err)
	{
		case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
			rd_kafka_assign(rk, partitions);
			break;

		case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
			rd_kafka_assign(rk, NULL);
			break;

		default:
			rd_kafka_assign(rk, NULL);
			break;
	}
}


/**
 * Message delivery report callback.
 * Called once for each message.
 * See rdkafka.h for more information.
 */
static void msg_delivered (rd_kafka_t *rk,
			   void *payload, size_t len,
			   rd_kafka_resp_err_t error_code,
			   void *opaque, void *msg_opaque) 
{
	if(msg_opaque)
	{
		ACK* ack = (ACK*)msg_opaque;

		prio::nextTick( [ack,error_code]() 
		{
			if(error_code)
			{
				ack->p.reject(KafkaEx(error_code));
			}
			else
			{
				ack->p.resolve();
			}

			delete ack;
		});
		return;
	}

#ifdef MOL_PROMISE_DEBUG
	if (error_code)
		fprintf(stderr, "%% Message delivery failed: %s\n",
			rd_kafka_err2str(error_code));
	else
		fprintf(stderr, "%% Message delivered (%zd bytes): %.*s\n", len,
			(int)len, (const char *)payload);
#endif
}



KafkaConfig::KafkaConfig()
	:brokers_("localhost:9092")
{
}

KafkaConfig::KafkaConfig(const std::string& brokers)
	:brokers_(brokers)
{
}

KafkaConfig::~KafkaConfig()
{}

rd_kafka_conf_t* KafkaConfig::handle()
{
	char errstr[512];

	rd_kafka_conf_t* conf = nullptr;
	conf = rd_kafka_conf_new();
	//rd_kafka_conf_set_log_cb(conf, logger);

	for( auto& prop: props_ )
	{	
#ifdef MOL_PROMISE_DEBUG
		std::cout << "set prop " << prop.first << " : " << prop.second << std::endl;
#endif		
		if (rd_kafka_conf_set(conf, prop.first.c_str(), prop.second.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) 
		{
			throw KafkaEx(errstr);
		}		
	}
	return conf;
}

void KafkaConfig::prop(const std::string& key, const std::string& val)
{
	props_.push_back(std::make_pair(key,val));
}

std::string KafkaConfig::brokers()
{
	return brokers_;
}

	
rd_kafka_topic_conf_t* KafkaTopicConfig::handle()
{
	char errstr[512];
	rd_kafka_topic_conf_t* conf = rd_kafka_topic_conf_new();

	for( auto& prop: props_ )
	{
#ifdef MOL_PROMISE_DEBUG
		std::cout << "set topic prop " << prop.first << " : " << prop.second << std::endl;
#endif		
		int res = rd_kafka_topic_conf_set(
			conf,
			prop.first.c_str(),
			prop.second.c_str(),
			errstr,
			sizeof(errstr)
		);
	}
	return conf;
}	

void KafkaTopicConfig::prop(const std::string& key, const std::string& val)
{
	props_.push_back(std::make_pair(key,val));
}


KafkaTopic::KafkaTopic(const std::string& name,rd_kafka_topic_t *rk)
	: name_(name), 
		rkt_producer_(rk,[](rd_kafka_topic_t* rkt) { rd_kafka_topic_destroy(rkt); })
{}

KafkaTopic::~KafkaTopic()
{}

std::string KafkaTopic::name()
{
	return name_;
}

rd_kafka_topic_t* KafkaTopic::handle()
{
	return rkt_producer_.get();
}


Kafka::Kafka(KafkaConfig& conf)
	:stop_(false)
{
	rd_kafka_conf_t* config = conf.handle();

	rd_kafka_conf_set_dr_cb(config, msg_delivered);

	char errstr[512];

	// producer
	if (!(rk_producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, config,
				errstr, sizeof(errstr)))) 
	{
		throw KafkaEx(errstr);
	}


	if (rd_kafka_brokers_add(rk_producer_, conf.brokers().c_str()) == 0) 
	{
		throw KafkaEx("no valid brokers specified");
	}	

	// consumer
	config = conf.handle();
	rd_kafka_conf_set_dr_cb(config, msg_delivered);
	rd_kafka_conf_set_rebalance_cb(config, rebalance_cb);

	if (!(rk_consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, config,
				errstr, sizeof(errstr)))) 
	{
		throw KafkaEx(errstr);
	}


	if (rd_kafka_brokers_add(rk_consumer_, conf.brokers().c_str()) == 0) 
	{
		throw KafkaEx(errstr);
	}		

	rd_kafka_poll_set_consumer(rk_consumer_);			
}

Kafka::~Kafka()
{
	stop();

	subscriptions_.clear();

	if(rk_producer_)
		rd_kafka_destroy(rk_producer_);	

	if(rk_consumer_)
		rd_kafka_destroy(rk_consumer_);	

	/* Let background threads clean up and terminate cleanly. */
	int run = 5;
	while (run-- > 0 && rd_kafka_wait_destroyed(1000) == -1)
		printf("Waiting for librdkafka to decommission\n");
	if (run <= 0)
		rd_kafka_dump(stdout, rk_producer_);	

	worker_.join();	
}

void Kafka::connect()
{
	worker_ = std::thread(&Kafka::poll,this);
}

void Kafka::consume()
{
	topics_list_ = rd_kafka_topic_partition_list_new(subscriptions_.size());
	int32_t partition = -1;

	for( auto& s : subscriptions_ )
	{
		rd_kafka_topic_partition_list_add(topics_list_, s.first.c_str(), partition);
	}

	rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

	if ((err = rd_kafka_subscribe(rk_consumer_, topics_list_))) 
	{
		throw KafkaEx(err);
	}

	rd_kafka_topic_partition_list_t * partitions = nullptr;
	while(true)
	{
		if (rd_kafka_outq_len(rk_producer_) > 0)
		{
			rd_kafka_poll(rk_producer_, 100);
		}

		rd_kafka_message_t *rkmessage = nullptr;
		rkmessage = rd_kafka_consumer_poll(rk_consumer_, 100);
		if (rkmessage) 
		{
			msg_consume(rkmessage);
			rd_kafka_message_destroy(rkmessage);
		}

		err = rd_kafka_assignment (rk_consumer_, &partitions);
		if(err != RD_KAFKA_RESP_ERR_NO_ERROR) 
		{
			std::cout << "_";
			std::cout.flush();
			usleep(100);
			continue;
		}
		if(partitions)
		{
			if( partitions->cnt > 0)
			{
				break;
			}
		}
		usleep(1000);
		std::cout << ".";
		std::cout.flush();
	};
	std::cout << " go kafka go" << std::endl;
	if(partitions)
	{
		rd_kafka_topic_partition_list_destroy(partitions);
	}
}	

void Kafka::stop()
{
	stop_ = true; 
	if(rk_consumer_)
		rd_kafka_consumer_close(rk_consumer_);
}

repro::Future<KafkaMsg> Kafka::subscribe(const std::string& topic)
{
	KafkaSubscription sub(topic);
	subscriptions_[topic] = sub;
	return sub.p.future();
}

void Kafka::create_topic(const std::string& topic)
{
	KafkaTopicConfig topic_conf;
	rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk_producer_, topic.c_str(), topic_conf.handle());

	topics_[topic] = KafkaTopic(topic,rkt);
}

void Kafka::create_topic(const std::string& topic, KafkaTopicConfig& topic_conf )
{
	rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk_producer_, topic.c_str(), topic_conf.handle());

	topics_[topic] = KafkaTopic(topic,rkt);
}

repro::Future<> Kafka::send(const std::string& topic,const std::string& msg, int partition )
{
	rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

	while( topics_.count(topic) == 0)
	{
		create_topic(topic);
	}

	rd_kafka_topic_t* rkt = topics_[topic].handle();

	ACK* ack = new ACK;

	char errstr[512];
	if (rd_kafka_produce(
			rkt, partition,
			RD_KAFKA_MSG_F_COPY,
			(void*)(msg.c_str()), msg.size(),
			NULL, 0,
			ack) == -1) 
	{
		err = rd_kafka_last_error();
	}
	if (err) 
	{
		ack->p.reject(KafkaEx(err));
		delete ack;
	}	

	return ack->p.future();	
}

repro::Future<> Kafka::send(const std::string& topic,const std::string& msg, const std::string& key, int partition )
{
	rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

	while( topics_.count(topic) == 0)
	{
		create_topic(topic);
	}

	rd_kafka_topic_t* rkt = topics_[topic].handle();	

	ACK* ack = new ACK;	

	char errstr[512];
	if (rd_kafka_produce(
			rkt, partition,
			RD_KAFKA_MSG_F_COPY,
			(void*)(msg.c_str()), msg.size(),
			key.c_str(), key.size(),
			ack) == -1) 
	{
		err = rd_kafka_last_error();
	}
	if (err) 
	{
		ack->p.reject(KafkaEx(err));
		delete ack;
	}		
}


void Kafka::msg_consume (rd_kafka_message_t *rkmessage) 
{
	if (rkmessage->err) 
	{
		if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) 
		{
#ifdef MOL_PROMISE_DEBUG			
			fprintf(stderr,
				"%% Consumer reached end of %s [] "
				"message queue at offset \n",
				rd_kafka_topic_name(rkmessage->rkt)
			);
#endif
			return;
		}

#ifdef MOL_PROMISE_DEBUG			
		fprintf(stderr, "%% Consume error for topic %s : %s\n",
			rd_kafka_topic_name(rkmessage->rkt),
			rd_kafka_message_errstr(rkmessage)
		);
#endif

		if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
			rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
		{
			return;
		}
	}

	rd_kafka_timestamp_type_t tstype;
	int64_t timestamp;
	//rd_kafka_headers_t *hdrs;

/*
	timestamp = rd_kafka_message_timestamp(rkmessage, &tstype);
	if (tstype != RD_KAFKA_TIMESTAMP_NOT_AVAILABLE) 
	{
		const char *tsname = "?";
		if (tstype == RD_KAFKA_TIMESTAMP_CREATE_TIME)
			tsname = "create time";
		else if (tstype == RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME)
			tsname = "log append time";

		fprintf(stdout, "%% Message timestamp: %s  (%ds ago)\n",
			tsname, timestamp,
			!timestamp ? 0 :
			(int)time(NULL) - (int)(timestamp/1000));
	}

	if (rkmessage->key_len) 
	{
		printf("Key: %.*s\n",
		(int)rkmessage->key_len, (char *)rkmessage->key);
	}

	//printf("%.*s\n",(int)rkmessage->len, (char *)rkmessage->payload);
*/
	std::string topic = rd_kafka_topic_name(rkmessage->rkt);

	if ( subscriptions_.count(topic) > 0 )
	{
		std::string payload;
		if(rkmessage->len)
		{
			payload = std::string((char*)rkmessage->payload,rkmessage->len);
		}

		std::string key;
		if (rkmessage->key_len)
		{
			key = std::string((char*)rkmessage->key,rkmessage->key_len);
		}

		KafkaMsg msg{topic,payload,key};

		nextTick( [this,msg]()
		{
			subscriptions_[msg.topic].p.resolve(msg);
		});
	}
}


void Kafka::poll()
{
	rd_kafka_message_t *rkmessage = nullptr;

	while(!stop_)
	{
		if (rd_kafka_outq_len(rk_producer_) > 0)
		{
			rd_kafka_poll(rk_producer_, 100);
		}

		rkmessage = rd_kafka_consumer_poll(rk_consumer_, 100);
		if (rkmessage) 
		{
			msg_consume(rkmessage);
			rd_kafka_message_destroy(rkmessage);
		}

		usleep(10);
	}
}

} // close namespace

