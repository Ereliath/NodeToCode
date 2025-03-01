// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "LLM/Providers/N2COpenAIService.h"

#include "LLM/N2CHttpHandler.h"
#include "LLM/N2CHttpHandlerBase.h"
#include "LLM/N2CLLMModels.h"
#include "LLM/N2CResponseParserBase.h"
#include "LLM/N2CSystemPromptManager.h"
#include "Utils/N2CLogger.h"

bool UN2COpenAIService::Initialize(const FN2CLLMConfig& InConfig)
{
    Config = InConfig;

    // Use default endpoint if none provided                                                                                                                                                              
    if (Config.ApiEndpoint.IsEmpty())                                                                                                                                                                     
    {                                                                                                                                                                                                     
        Config.ApiEndpoint = DefaultEndpoint;                                                                                                                                                             
    }

    // Create HTTP handler
    HttpHandler = NewObject<UN2CHttpHandler>(this);
    if (!HttpHandler)
    {
        FN2CLogger::Get().LogError(TEXT("Failed to create HTTP handler"), TEXT("OpenAIService"));
        return false;
    }
    HttpHandler->Initialize(Config);

    // Create OpenAI response parser
    ResponseParser = NewObject<UN2COpenAIResponseParser>(this);
    if (!ResponseParser)
    {
        FN2CLogger::Get().LogError(TEXT("Failed to create OpenAI response parser"), TEXT("OpenAIService"));
        return false;
    }
    ResponseParser->Initialize();

    // Create system prompt manager
    PromptManager = NewObject<UN2CSystemPromptManager>(this);
    if (!PromptManager)
    {
        FN2CLogger::Get().LogError(TEXT("Failed to create system prompt manager"), TEXT("OpenAIService"));
        return false;
    }
    PromptManager->Initialize(Config);

    // Set required headers
    TMap<FString, FString> Headers;
    GetProviderHeaders(Headers);
    HttpHandler->ExtraHeaders = Headers;

    bIsInitialized = true;
    return true;
}

void UN2COpenAIService::SendRequest(
    const FString& JsonPayload,
    const FString& SystemMessage,
    const FOnLLMResponseReceived& OnComplete)
{
    if (!bIsInitialized)
    {
        FN2CLogger::Get().LogError(TEXT("Service not initialized"), TEXT("OpenAIService"));
        const bool bExecuted = OnComplete.ExecuteIfBound(TEXT("{\"error\": \"Service not initialized\"}"));
        return;
    }
    
    // Log provider and model info
    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Sending request to OpenAI using model: %s"), *Config.Model),
        EN2CLogSeverity::Info,
        TEXT("OpenAIService")
    );

    // Format request payload for OpenAI
    FString FormattedPayload = FormatRequestPayload(JsonPayload, SystemMessage);

    // Send request through HTTP handler
    HttpHandler->PostLLMRequest(
        Config.ApiEndpoint,
        Config.ApiKey,
        FormattedPayload,
        OnComplete
    );
}

void UN2COpenAIService::GetConfiguration(
    FString& OutEndpoint,
    FString& OutAuthToken,
    bool& OutSupportsSystemPrompts)
{
    OutEndpoint = Config.ApiEndpoint;
    OutAuthToken = Config.ApiKey;
    
    // Find matching model enum for system prompt support check
    for (int32 i = 0; i < static_cast<int32>(EN2COpenAIModel::GPT_o1_Mini) + 1; i++)
    {
        EN2COpenAIModel Model = static_cast<EN2COpenAIModel>(i);
        if (FN2CLLMModelUtils::GetOpenAIModelValue(Model) == Config.Model)
        {
            OutSupportsSystemPrompts = FN2CLLMModelUtils::SupportsSystemPrompts(Model);
            return;
        }
    }
    
    // Default to supporting system prompts if model not found
    OutSupportsSystemPrompts = true;
}

void UN2COpenAIService::GetProviderHeaders(TMap<FString, FString>& OutHeaders) const
{
    OutHeaders.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
    OutHeaders.Add(TEXT("Content-Type"), TEXT("application/json"));
    
    // Add organization header if available
    if (!OrganizationId.IsEmpty())
    {
        OutHeaders.Add(TEXT("OpenAI-Organization"), OrganizationId);
    }
}

FString UN2COpenAIService::FormatRequestPayload(const FString& UserMessage, const FString& SystemMessage) const
{
    // Check if model supports system prompts
    bool bSupportsSystemPrompts = false;
    for (int32 i = 0; i < static_cast<int32>(EN2COpenAIModel::GPT_o1_Mini) + 1; i++)
    {
        EN2COpenAIModel Model = static_cast<EN2COpenAIModel>(i);
        if (FN2CLLMModelUtils::GetOpenAIModelValue(Model) == Config.Model)
        {
            bSupportsSystemPrompts = FN2CLLMModelUtils::SupportsSystemPrompts(Model);
            break;
        }
    }

    // Determine final content based on system prompt support
    FString FinalContent;
    if (bSupportsSystemPrompts)
    {
        FinalContent = UserMessage;
    }
    else
    {
        // Only merge if not already merged
        FinalContent = PromptManager->MergePrompts(UserMessage, SystemMessage);
    }

    // Try prepending source files to the user message
    PromptManager->PrependSourceFilesToUserMessage(FinalContent);

    // Define the JSON schema for our response format
    const FString JsonSchema = TEXT(R"(
  {
    "name": "n2c_translation",
    "schema": 
      {
        "type": "object",
        "properties":
        {
            "graphs": 
            {
                "type": "array",
                "items":
                {
                    "type": "object",
                    "properties":
                {
                        "graph_name": {"type": "string"},
                        "graph_type": {"type": "string"},
                        "graph_class": {"type": "string"},
                        "code":
                        {
                            "type": "object",
                            "properties":
                            {
                                "graphDeclaration": {"type": "string"},
                                "graphImplementation": {"type": "string"},
                                "implementationNotes": {"type": "string"}
                            },
                            "required": ["graphDeclaration", "graphImplementation"]
                        }
                    },
                    "required": ["graph_name", "graph_type", "graph_class", "code"]
                }
            }
        },
        "required": ["graphs"],
        "additionalProperties": false
    }
  })");

    // Build JSON payload structure using JSON API
    TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
    RootObject->SetStringField(TEXT("model"), Config.Model);

    // Parse the schema string into a JSON object
    TSharedPtr<FJsonObject> SchemaObject;
    TSharedRef<TJsonReader<>> SchemaReader = TJsonReaderFactory<>::Create(JsonSchema);
    if (!FJsonSerializer::Deserialize(SchemaReader, SchemaObject))
    {
        FN2CLogger::Get().LogError(TEXT("Failed to parse JSON schema"), TEXT("OpenAIService"));
        return FString();
    }

    // Build messages array
    TArray<TSharedPtr<FJsonValue>> MessagesArray;

    // Add json schema to response object if model supports structured outputs
    if (Config.Model != "o1-preview-2024-09-12" || "o1-mini-2024-09-12")
    {
        // Add response format with parsed schema
        TSharedPtr<FJsonObject> ResponseFormatObject = MakeShared<FJsonObject>();
        ResponseFormatObject->SetStringField(TEXT("type"), TEXT("json_schema"));
        ResponseFormatObject->SetObjectField(TEXT("json_schema"), SchemaObject);
        RootObject->SetObjectField(TEXT("response_format"), ResponseFormatObject);
    }

    // Add system prompt to system message object if model supports system prompts
    if (bSupportsSystemPrompts)
    {
        RootObject->SetNumberField(TEXT("temperature"), 0.0);
        RootObject->SetNumberField(TEXT("max_tokens"), 8192);
        
        // Add system message
        TSharedPtr<FJsonObject> SystemMessageObject = MakeShared<FJsonObject>();
        SystemMessageObject->SetStringField(TEXT("role"), TEXT("system"));
        SystemMessageObject->SetStringField(TEXT("content"), SystemMessage);
        MessagesArray.Add(MakeShared<FJsonValueObject>(SystemMessageObject));
    }

    // Add user message
    TSharedPtr<FJsonObject> UserMessageObject = MakeShared<FJsonObject>();
    UserMessageObject->SetStringField(TEXT("role"), TEXT("user"));
    UserMessageObject->SetStringField(TEXT("content"), FinalContent);
    MessagesArray.Add(MakeShared<FJsonValueObject>(UserMessageObject));

    RootObject->SetArrayField(TEXT("messages"), MessagesArray);

    // Serialize JSON to string
    FString Payload;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
    FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

    FN2CLogger::Get().Log(FString::Printf(TEXT("LLM Request Payload:\n\n%s"), *Payload), EN2CLogSeverity::Debug);
    
    return Payload;
}
