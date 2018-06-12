#include "gtest/gtest.h"
#include <memory>
#include <list>
#include <utility>
#include <iostream>
#include <string>
#include <exception>
#include <functional>
#include "test.h"
#include "priocpp/api.h"
#include "priocpp/task.h"
#include <signal.h>
#include <reprokafka/kafka.h>
 
using namespace prio;
using namespace reprokafka;


class BasicTest : public ::testing::Test {
 protected:

  static void SetUpTestCase() {


  }

  virtual void SetUp() {
	 // MOL_TEST_PRINT_CNTS();
  }

  virtual void TearDown() {
	  MOL_TEST_PRINT_CNTS();
  }

}; // end test setup



#ifdef _RESUMABLE_FUNCTIONS_SUPPORTED_XX

repro::Future<> coroutine_example(reproredis::RedisPool& redis, std::string& result);


TEST_F(BasicTest, Coroutine) {

	std::string result;
	{
		signal(SIGINT).then([](int s) {theLoop().exit(); });

		reproredis::RedisPool Redis("redis://localhost:6379/", 4);

		coroutine_example(Redis, result)
		.then([]() 
		{
			std::cout << "going down" << std::endl;

			timeout([]() {
				std::cout << "finis" << std::endl;
				theLoop().exit();
			}, 1, 0);
		}).otherwise([](const std::exception& ex) {});

		theLoop().run();
	}

	EXPECT_EQ("promised", result);
	MOL_TEST_ASSERT_CNTS(0, 0);
}


repro::Future<> coroutine_example(reproredis::RedisPool& redis, std::string& result)
{
	try
	{
		reproredis::RedisResult::Ptr r = co_await redis.cmd("SET", "promise-test", "promised");

		std::cout << "did set" << std::endl;

		reproredis::RedisResult::Ptr r2 = co_await redis.cmd("GET", "promise-test");

		std::cout << "did got " << result << std::endl;
		result = r2->str();
		std::cout << "did get " << result << std::endl;


	}
	catch (const std::exception& ex)
	{
		std::cout << "ex:t " << ex.what() << std::endl;
		theLoop().exit();
	}
	std::cout << "coro end" << std::endl;

	co_return;
}

#endif



TEST_F(BasicTest, KafkaTest) 
{
	std::string result;

	{
		KafkaConfig conf;
		KafkaTopicConfig topicConf;

		conf.prop("group.id","mytopicgid");
		Kafka kConsumer(conf);

		signal(SIGINT).then([](int s) { theLoop().exit(); });
		
		//timeout( [&kConsumer,&result]() 
		//{
			kConsumer
			.subscribe("mytopic")
			.then([&result,&kConsumer](KafkaMsg msg)
			{
				std::cout << "!!" << msg.topic << ": " << msg.msg << std::endl;
				result = msg.msg;
				timeout([]()
				{
					theLoop().exit();
				},1,0);
			});

		//},0,100);

			kConsumer.consume();
		kConsumer.connect();//0,RD_KAFKA_OFFSET_STORED);//,hilo.first);	

		timeout( [&kConsumer,&result]() 
		{
			kConsumer.create_topic("mytopic");
			kConsumer
			.send("mytopic","killroy was here!")
			.then([]()
			{
				std::cout << "ACK!" << std::endl;
			});
		},5,0);
	//	std::cout << low << ":" << hilo.second << std::endl;
	
		std::cout << "loop start" << std::endl;
		theLoop().run();
		std::cout << "loop end" << std::endl;
	}
	EXPECT_EQ("killroy was here!", result);
	MOL_TEST_ASSERT_CNTS(0, 0);
}



int main(int argc, char **argv) {

	prio::init();

    ::testing::InitGoogleTest(&argc, argv);
    int r = RUN_ALL_TESTS();

    return r;
}
