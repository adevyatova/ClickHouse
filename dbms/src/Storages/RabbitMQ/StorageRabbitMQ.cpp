#include <Storages/RabbitMQ/StorageRabbitMQ.h>

#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/LimitBlockInputStream.h>
#include <DataStreams/UnionBlockInputStream.h>
#include <DataStreams/copyData.h>

#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeString.h>

#include <Interpreters/InterpreterInsertQuery.h>
#include <Interpreters/evaluateConstantExpression.h>

#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTInsertQuery.h>
#include <Parsers/ASTLiteral.h>

#include <Storages/RabbitMQ/RabbitMQSettings.h>
#include <Storages/RabbitMQ/RabbitMQBlockInputStream.h>
#include <Storages/RabbitMQ/RabbitMQBlockOutputStream.h>
#include <Storages/RabbitMQ/WriteBufferToRabbitMQProducer.h>
#include <Storages/RabbitMQ/RabbitMQHandler.h>

#include <Storages/StorageFactory.h>
#include <Storages/StorageMaterializedView.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <Common/Exception.h>
#include <Common/Macros.h>
#include <Common/config_version.h>
#include <Common/setThreadName.h>
#include <Common/typeid_cast.h>
#include <common/logger_useful.h>
#include <Common/quoteString.h>
#include <Common/parseAddress.h>

#include <amqpcpp.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int UNKNOWN_EXCEPTION;
    extern const int CANNOT_READ_FROM_ISTREAM;
    extern const int INVALID_CONFIG_PARAMETER;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int UNSUPPORTED_METHOD;
    extern const int UNKNOWN_SETTING;
    extern const int READONLY_SETTING;
}

StorageRabbitMQ::StorageRabbitMQ(
        const StorageID & table_id_,
        Context & context_,
        const ColumnsDescription & columns_,
        const String & host_port_,
        const Names & routing_keys_,
        const String & format_name_,
        char row_delimiter_,
        size_t num_consumers_,
        UInt64 max_block_size_,
        size_t skip_broken_)
        : IStorage(table_id_,
        ColumnsDescription({{"_topic", std::make_shared<DataTypeString>()},
                            {"_key", std::make_shared<DataTypeString>()},
                            {"_offset", std::make_shared<DataTypeUInt64>()},
                            {"_partition", std::make_shared<DataTypeUInt64>()},
                            {"_timestamp", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTime>())}}, true))
        , global_context(context_.getGlobalContext())
        , host_port(global_context.getMacros()->expand(host_port_))
        , routing_keys(global_context.getMacros()->expand(routing_keys_))
        , connection_handler(parseAddress(host_port, 5672))
        , connection(&connection_handler,
                AMQP::Login(connection_handler.get_user_name(), connection_handler.get_password()))
        , format_name(global_context.getMacros()->expand(format_name_))
        , row_delimiter(row_delimiter_)
        , num_consumers(num_consumers_)
        , max_block_size(max_block_size_)
        , skip_broken(skip_broken_)
        , log(&Logger::get("StorageRabbitMQ (" + table_id_.table_name + ")"))
        , semaphore(0, num_consumers_)
{
    setColumns(columns_);
}

BlockInputStreams StorageRabbitMQ::read(
        const Names & column_names,
        const SelectQueryInfo & /* query_info */,
        const Context & context,
        QueryProcessingStage::Enum /* processed_stage */,
        size_t /* max_block_size */,
        unsigned /* num_streams */)
{
    if (num_created_consumers == 0)
        return BlockInputStreams();

    BlockInputStreams streams;
    streams.reserve(num_created_consumers);

    for (size_t i = 0; i < num_created_consumers; ++i)
    {
        streams.emplace_back(std::make_shared<RabbitMQBlockInputStream>(*this, context, column_names));
    }

    LOG_DEBUG(log, "Starting reading " << streams.size() << " streams");
    return streams;
}

BlockOutputStreamPtr StorageRabbitMQ::write(const ASTPtr &, const Context & context)
{
    return std::make_shared<RabbitMQBlockOutputStream>(*this, context);
}

void StorageRabbitMQ::startup()
{
    for (size_t i = 0; i < num_consumers; ++i)
    {
        try
        {
            pushReadBuffer(createReadBuffer());
            ++num_created_consumers;
        }
        catch (const AMQP::Exception &)
        {
            tryLogCurrentException(log);
        }
    }
}

void StorageRabbitMQ::shutdown()
{
    for (size_t i = 0; i < num_created_consumers; ++i)
    {
        auto buffer = popReadBuffer();
    }
}

void StorageRabbitMQ::pushReadBuffer(ConsumerBufferPtr buffer)
{
    buffers.push_back(buffer);
}


ConsumerBufferPtr StorageRabbitMQ::popReadBuffer()
{
    return popReadBuffer(std::chrono::milliseconds::zero());
}


ConsumerBufferPtr StorageRabbitMQ::popReadBuffer(std::chrono::milliseconds timeout)
{
    // Wait for the first free buffer
    if (timeout == std::chrono::milliseconds::zero())
        semaphore.wait();
    else
    {
        if (!semaphore.tryWait(timeout.count()))
            return nullptr;
    }
    // Take the first available buffer from the list
    auto buffer = buffers.back();
    buffers.pop_back();
    return buffer;
}


ProducerBufferPtr StorageRabbitMQ::createWriteBuffer()
{
    auto producer = std::make_shared<AMQP::Channel>(&connection);

    return std::make_shared<WriteBufferToRabbitMQProducer>(
            producer, &connection_handler, routing_keys[0],
            row_delimiter ? std::optional<char>{row_delimiter} : std::optional<char>(), 1, 1024);
}


ConsumerBufferPtr StorageRabbitMQ::createReadBuffer()
{
    auto consumer = std::make_shared<AMQP::Channel>(&connection);

    return std::make_shared<ReadBufferFromRabbitMQConsumer>(consumer, &connection_handler);
}


void registerStorageRabbitMQ(StorageFactory & factory)
{
    factory.registerStorage("RabbitMQ", [](const StorageFactory::Arguments & args)
    {
        ASTs & engine_args = args.engine_args;
        size_t args_count = engine_args.size();
        bool has_settings = args.storage_def->settings;

        /** Arguments of engine is following:
              * - RabbitMQ host:port (default: localhost:5672)
              * - List of routing keys to bind producer->exchange->queue <-> consumer (default: "")
              * optional for now:
              * - Number of consumers
              * - Message format (string)
              * - Row delimiter
              * - Max block size for background consumption
              * - Skip (at least) unreadable messages number
              */

        RabbitMQSettings rabbitmq_settings;
        if (has_settings)
        {
            rabbitmq_settings.loadFromQuery(*args.storage_def);
        }

        if (args_count < 2 || args_count > 7)
        {
            throw Exception(
                    "Storage RabbitMQ requires 2-7 parameters!", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
        }

        String host_port = rabbitmq_settings.rabbitmq_host_port;
        if (args_count >= 1)
        {
            const auto * ast = engine_args[0]->as<ASTLiteral>();
            if (ast && ast->value.getType() == Field::Types::String)
            {
                host_port = safeGet<String>(ast->value);
            }
            else
            {
                throw Exception(String("RabbitMQ host:port must be a string"), ErrorCodes::BAD_ARGUMENTS);
            }
        }

        String routing_key_list = rabbitmq_settings.rabbitmq_routing_key_list.value;
        if (args_count >= 2)
        {
            engine_args[1] = evaluateConstantExpressionAsLiteral(engine_args[1], args.local_context);
            routing_key_list = engine_args[1]->as<ASTLiteral &>().value.safeGet<String>();
        }

        Names routing_keys;
        boost::split(routing_keys, routing_key_list, [](char c){ return c == ','; });
        for (String & key : routing_keys)
        {
            boost::trim(key);
        }

        UInt64 num_consumers = rabbitmq_settings.rabbitmq_num_consumers;
        if (args_count >= 3)
        {
            const auto * ast = engine_args[2]->as<ASTLiteral>();
            if (ast && ast->value.getType() == Field::Types::UInt64)
            {
                num_consumers = safeGet<UInt64>(ast->value);
            }
            else
            {
                throw Exception("Number of consumers must be a positive integer", ErrorCodes::BAD_ARGUMENTS);
            }
        }


        String format = rabbitmq_settings.rabbitmq_format.value;
        if (args_count >= 4)
        {
            engine_args[3] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[3], args.local_context);

            const auto * ast = engine_args[3]->as<ASTLiteral>();
            if (ast && ast->value.getType() == Field::Types::String)
            {
                format = safeGet<String>(ast->value);
            }
            else
            {
                throw Exception("Format must be a string", ErrorCodes::BAD_ARGUMENTS);
            }
        }

        char row_delimiter = rabbitmq_settings.rabbitmq_row_delimiter;
        if (args_count >= 5)
        {
            engine_args[4] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[4], args.local_context);

            const auto * ast = engine_args[4]->as<ASTLiteral>();
            String arg;
            if (ast && ast->value.getType() == Field::Types::String)
            {
                arg = safeGet<String>(ast->value);
            }
            else
            {
                throw Exception("Row delimiter must be a char", ErrorCodes::BAD_ARGUMENTS);
            }
            if (arg.size() > 1)
            {
                throw Exception("Row delimiter must be a char", ErrorCodes::BAD_ARGUMENTS);
            }
            else if (arg.size() == 0)
            {
                row_delimiter = '\0';
            }
            else
            {
                row_delimiter = arg[0];
            }
        }

        UInt64 max_block_size = static_cast<size_t>(rabbitmq_settings.rabbitmq_max_block_size);
        if (args_count >= 6)
        {
            const auto * ast = engine_args[5]->as<ASTLiteral>();
            if (ast && ast->value.getType() == Field::Types::UInt64)
            {
                max_block_size = static_cast<size_t>(safeGet<UInt64>(ast->value));
            }
            else
            {
                throw Exception("Maximum block size must be a positive integer", ErrorCodes::BAD_ARGUMENTS);
            }
        }

        size_t skip_broken = static_cast<size_t>(rabbitmq_settings.rabbitmq_skip_broken_messages);
        if (args_count >= 7)
        {
            const auto * ast = engine_args[6]->as<ASTLiteral>();
            if (ast && ast->value.getType() == Field::Types::UInt64)
            {
                skip_broken = static_cast<size_t>(safeGet<UInt64>(ast->value));
            }
            else
            {
                throw Exception("Number of broken messages to skip must be a non-negative integer", ErrorCodes::BAD_ARGUMENTS);
            }
        }

        return StorageRabbitMQ::create(
                args.table_id, args.context, args.columns,
                host_port, routing_keys,
                format, row_delimiter, num_consumers, max_block_size, skip_broken);
    });
}
}
