//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// usp_reco_engine_adapter.cpp: Implementation definitions for CSpxUspRecoEngineAdapter C++ class
//

#include "stdafx.h"
#include "usp_reco_engine_adapter.h"
#include "handle_table.h"
#include "file_utils.h"
#include <inttypes.h>
#include <cstring>
#include "service_helpers.h"
#include "exception.h"


namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxUspRecoEngineAdapter::CSpxUspRecoEngineAdapter() :
    m_audioState(AudioState::Idle),
    m_uspState(UspState::Idle),
    m_servicePreferedBufferSizeSendingNow(0),
    m_bytesInBuffer(0),
    m_ptrIntoBuffer(nullptr),
    m_bytesLeftInBuffer(0)
{
}

CSpxUspRecoEngineAdapter::~CSpxUspRecoEngineAdapter()
{
    SPX_DBG_TRACE_FUNCTION();
}

void CSpxUspRecoEngineAdapter::Init()
{
    SPX_DBG_TRACE_FUNCTION();
    SPX_IFTRUE_THROW_HR(GetSite() == nullptr, SPXERR_UNINITIALIZED);
    SPX_IFTRUE_THROW_HR(m_handle != nullptr, SPXERR_ALREADY_INITIALIZED);
    SPX_DBG_ASSERT(IsState(AudioState::Idle) && IsState(UspState::Idle));
}

void CSpxUspRecoEngineAdapter::Term()
{
    SPX_DBG_TRACE_SCOPE("Terminating CSpxUspRecoEngineAdapter...", "Terminating CSpxUspRecoEngineAdapter... Done!");

    WriteLock_Type writeLock(m_stateMutex);
    if (ChangeState(UspState::Terminating))
    {
        writeLock.unlock();

        SPX_DBG_TRACE_VERBOSE("%s: Terminating USP Connection (0x%x)", __FUNCTION__, m_handle.get());
        m_handle.reset();

        writeLock.lock();
        ChangeState(UspState::Zombie);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::SetAdapterMode(bool singleShot)
{
    SPX_DBG_TRACE_VERBOSE("%s: singleShot=%d", __FUNCTION__, singleShot);
    m_singleShot = singleShot;
}

void CSpxUspRecoEngineAdapter::SetFormat(WAVEFORMATEX* pformat)
{
    SPX_DBG_TRACE_VERBOSE_IF(pformat == nullptr, "%s - pformat == nullptr", __FUNCTION__);
    SPX_DBG_TRACE_VERBOSE_IF(pformat != nullptr, "%s\n  wFormatTag:      %s\n  nChannels:       %d\n  nSamplesPerSec:  %d\n  nAvgBytesPerSec: %d\n  nBlockAlign:     %d\n  wBitsPerSample:  %d\n  cbSize:          %d",
        __FUNCTION__,
        pformat->wFormatTag == WAVE_FORMAT_PCM ? "PCM" : std::to_string(pformat->wFormatTag).c_str(),
        pformat->nChannels,
        pformat->nSamplesPerSec,
        pformat->nAvgBytesPerSec,
        pformat->nBlockAlign,
        pformat->wBitsPerSample,
        pformat->cbSize);

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState() && !IsState(UspState::Terminating))
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (pformat != nullptr && IsState(UspState::Idle) && ChangeState(AudioState::Idle, AudioState::Ready))
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: -> PrepareFirstAudioReadyState()", __FUNCTION__);
        PrepareFirstAudioReadyState(pformat);
    }
    else if (pformat == nullptr && (ChangeState(AudioState::Idle) || IsState(UspState::Terminating)))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterCompletedSetFormatStop()", __FUNCTION__);
        GetSite()->AdapterCompletedSetFormatStop(this);

        m_format.reset();
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::ProcessAudio(AudioData_Type data, uint32_t size)
{
    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (size > 0 && ChangeState(AudioState::Ready, UspState::Idle, AudioState::Sending, UspState::WaitingForTurnStart))
    {
        writeLock.unlock(); // SendPreAudioMessages() will use USP to send speech config/context ... don't hold the lock

        SPX_DBG_TRACE_VERBOSE_IF(1, "%s: SendPreAudioMessages() ... size=%d", __FUNCTION__, size);
        SendPreAudioMessages();
        UspWrite(data.get(), size);

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterStartingTurn()", __FUNCTION__);
        GetSite()->AdapterStartingTurn(this);
    }
    else if (size > 0 && IsState(AudioState::Sending))
    {
        writeLock.unlock(); // UspWrite() will use USP to send data... don't hold the lock

        SPX_DBG_TRACE_VERBOSE_IF(1, "%s: Sending Audio ... size=%d", __FUNCTION__, size);
        UspWrite(data.get(), size);
    }
    else if (size == 0 && IsState(AudioState::Sending))
    {
        writeLock.unlock(); // UspWrite_Flush() will use USP to send data... don't hold the lock

        SPX_DBG_TRACE_VERBOSE_IF(1, "%s: Flushing Audio ... size=0 USP-FLUSH", __FUNCTION__);
        UspWrite_Flush();
    }
    else if (!IsState(AudioState::Sending))
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE_IF(1, "%s: Ignoring audio size=%d ... (audioState/uspState=%d/%d)", __FUNCTION__, size, m_audioState, m_uspState);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::EnsureUspInit()
{
    if (m_handle == nullptr)
    {
        UspInitialize();
    }
}

void CSpxUspRecoEngineAdapter::UspInitialize()
{
    SPX_IFTRUE_THROW_HR(m_handle != nullptr, SPXERR_ALREADY_INITIALIZED);

    // Get the named property service...
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    SPX_IFTRUE_THROW_HR(properties == nullptr, SPXERR_UNEXPECTED_USP_SITE_FAILURE);

    // Set up the client configuration...
    USP::Client client(*this, USP::EndpointType::BingSpeech);
    SetUspEndpoint(properties, client);
    SetUspRecoMode(properties, client);
    SetUspAuthentication(properties, client);
    SPX_DBG_TRACE_VERBOSE("%s: recoMode=%d", __FUNCTION__, m_recoMode);

    // Finally ... Connect to the service
    m_handle = client.Connect();
}

USP::Client& CSpxUspRecoEngineAdapter::SetUspEndpoint(std::shared_ptr<ISpxNamedProperties>& properties, USP::Client& client)
{
    m_customEndpoint = false;

    auto endpoint = properties->GetStringValue(g_SPEECH_Endpoint);
    if (PAL::wcsicmp(endpoint.c_str(), L"CORTANA") == 0)    // Use the CORTANA SDK endpoint
    {
        return client.SetEndpointType(USP::EndpointType::CDSDK);
    }

    if (!endpoint.empty())                                  // Use the SPECIFIED endpoint
    {
        SPX_DBG_TRACE_VERBOSE("Using Custom URL: %ls", endpoint.c_str());

        m_customEndpoint = true;
        return client.SetEndpointUrl(PAL::ToString(endpoint));
    }

    // Then check translation
    auto fromLang = properties->GetStringValue(g_TRANSLATION_FromLanguage);
    if (!fromLang.empty())
    {
        auto toLangs = properties->GetStringValue(g_TRANSLATION_ToLanguages);
        SPX_IFTRUE_THROW_HR(toLangs.empty(), SPXERR_INVALID_ARG);
        auto voice = properties->GetStringValue(g_TRANSLATION_Voice);
        // Before unified service, we need modelId to run translation.
        auto customSpeechModelId = properties->GetStringValue(g_SPEECH_ModelId);

        return client.SetEndpointType(USP::EndpointType::Translation)
            .SetTranslationSourceLanguage(PAL::ToString(fromLang))
            .SetTranslationTargetLanguages(PAL::ToString(toLangs))
            .SetTranslationVoice(PAL::ToString(voice))
            // Todo: remove this when switch to unified service.
            .SetModelId(PAL::ToString(customSpeechModelId));
    }

    // Then check CRIS
    auto customSpeechModelId = properties->GetStringValue(g_SPEECH_ModelId);
    if (!customSpeechModelId.empty())                       // Use the Custom Speech Intelligent Service
    {
        return client.SetEndpointType(USP::EndpointType::Cris).SetModelId(PAL::ToString(customSpeechModelId));
    }

     // Otherwise ... Use the default SPEECH endpoints
    if (properties->HasStringValue(g_SPEECH_RecoLanguage))
    {
        // Get the property that indicates what language to use...
        auto value = properties->GetStringValue(g_SPEECH_RecoLanguage);
        return client.SetEndpointType(USP::EndpointType::BingSpeech).SetLanguage(PAL::ToString(value));
    }
    else 
    {
        return client.SetEndpointType(USP::EndpointType::BingSpeech);
    }
}

USP::Client& CSpxUspRecoEngineAdapter::SetUspRecoMode(std::shared_ptr<ISpxNamedProperties>& properties, USP::Client& client)
{
    USP::RecognitionMode mode = USP::RecognitionMode::Interactive;

    // Check mode in the property collection first.
    auto checkHr = GetRecoModeFromProperties(properties, mode);
    SPX_THROW_HR_IF(checkHr, SPX_FAILED(checkHr) && (checkHr != SPXERR_NOT_FOUND));

    // Check mode string in the custom URL if needed.
    if ((checkHr == SPXERR_NOT_FOUND) && m_customEndpoint)
    {
        SPX_DBG_TRACE_VERBOSE("%s: Check mode string in the Custom URL.", __FUNCTION__);

        auto endpoint = properties->GetStringValue(g_SPEECH_Endpoint);
        // endpoint should not be null if m_customeEndpoint is set.
        SPX_THROW_HR_IF(SPXERR_RUNTIME_ERROR, endpoint.empty());

        checkHr = GetRecoModeFromEndpoint(endpoint, mode);
        SPX_THROW_HR_IF(checkHr, SPX_FAILED(checkHr) && (checkHr != SPXERR_NOT_FOUND));
    }

    m_recoMode = mode;
    return client.SetRecognitionMode(m_recoMode);
}

USP::Client&  CSpxUspRecoEngineAdapter::SetUspAuthentication(std::shared_ptr<ISpxNamedProperties>& properties, USP::Client& client)
{
    // Get the properties that indicates what endpoint to use...
    auto uspSubscriptionKey = properties->GetStringValue(g_SPEECH_SubscriptionKey);
    auto uspAuthToken = properties->GetStringValue(g_SPEECH_AuthToken);
    auto uspRpsToken = properties->GetStringValue(g_SPEECH_RpsToken);

    // Use those properties to determine which authentication type to use
    if (!uspSubscriptionKey.empty())
    {
        return client.SetAuthentication(USP::AuthenticationType::SubscriptionKey, PAL::ToString(uspSubscriptionKey));
    }
    if (!uspAuthToken.empty())
    {
        return client.SetAuthentication(USP::AuthenticationType::AuthorizationToken, PAL::ToString(uspAuthToken));
    }
    if (!uspRpsToken.empty())
    {
        return client.SetAuthentication(USP::AuthenticationType::SearchDelegationRPSToken, PAL::ToString(uspRpsToken));
    }

    ThrowInvalidArgumentException("No Authentication parameters were specified.");
    
    return client; // fixes "not all control paths return a value"
}

SPXHR CSpxUspRecoEngineAdapter::GetRecoModeFromProperties(const std::shared_ptr<ISpxNamedProperties>& properties, USP::RecognitionMode& recoMode) const
{
     SPXHR hr = SPX_NOERROR;

    // Get the property that indicates what reco mode to use...
    auto value = properties->GetStringValue(g_SPEECH_RecoMode);

    if (value.empty())
    {
        hr = SPXERR_NOT_FOUND;
    }
    else if (PAL::wcsicmp(value.c_str(), g_SPEECH_RecoMode_Interactive) == 0)
    {
        recoMode = USP::RecognitionMode::Interactive;
    }
    else if (PAL::wcsicmp(value.c_str(), g_SPEECH_RecoMode_Conversation) == 0)
    {
        recoMode = USP::RecognitionMode::Conversation;
    }
    else if (PAL::wcsicmp(value.c_str(), g_SPEECH_RecoMode_Dictation) == 0)
    {
        recoMode = USP::RecognitionMode::Dictation;
    }
    else
    {
        SPX_DBG_ASSERT_WITH_MESSAGE(false, "Unknown RecognitionMode in USP::RecognitionMode.");
        LogError("Unknown RecognitionMode value %ls", value.c_str());
        hr = SPXERR_INVALID_ARG;
    }

    return hr;
}

SPXHR CSpxUspRecoEngineAdapter::GetRecoModeFromEndpoint(const std::wstring& endpoint, USP::RecognitionMode& recoMode)
{
    SPXHR hr = SPX_NOERROR;

    if (endpoint.find(L"/interactive/") != std::string::npos)
    {
        recoMode = USP::RecognitionMode::Interactive;
    }
    else if (endpoint.find(L"/conversation/") != std::string::npos)
    {
        recoMode = USP::RecognitionMode::Conversation;
    }
    else if (endpoint.find(L"/dictation/") != std::string::npos)
    {
        recoMode = USP::RecognitionMode::Dictation;
    }
    else
    {
        hr = SPXERR_NOT_FOUND;
    }

    return hr;
}

void CSpxUspRecoEngineAdapter::UspSendSpeechContext()
{
    // Get the Dgi json payload
    std::list<std::string> listenForList = GetListenForListFromSite();
    auto listenForJson = GetDgiJsonFromListenForList(listenForList);

    // Get the intent payload
    std::string provider, id, key;
    GetIntentInfoFromSite(provider, id, key);
    auto intentJson = GetLanguageUnderstandingJsonFromIntentInfo(provider, id, key);

    // Do we expect to receive an intent payload from the service?
    m_expectIntentResponse = !intentJson.empty();

    // Take the json payload and the intent payload, and create the speech context json
    auto speechContext = GetSpeechContextJson(listenForJson, intentJson);
    if (!speechContext.empty())
    {
        // Since it's not empty, we'll send it (empty means we don't have either dgi or intent payload)
        std::string messagePath = "speech.context";
        UspSendMessage(messagePath, speechContext);
    }
}

void CSpxUspRecoEngineAdapter::UspSendMessage(const std::string& messagePath, const std::string &buffer)
{
    SPX_DBG_TRACE_VERBOSE("%s='%s'", messagePath.c_str(), buffer.c_str());
    UspSendMessage(messagePath, (const uint8_t*)buffer.c_str(), buffer.length());
}

void CSpxUspRecoEngineAdapter::UspSendMessage(const std::string& messagePath, const uint8_t* buffer, size_t size)
{
    SPX_DBG_ASSERT(m_handle != nullptr || IsState(UspState::Terminating) || IsState(UspState::Zombie));
    if (!IsState(UspState::Terminating) && !IsState(UspState::Zombie) && m_handle != nullptr)
    {
        m_handle->SendMessage(messagePath, buffer, size);
    }
}

void CSpxUspRecoEngineAdapter::UspWriteFormat(WAVEFORMATEX* pformat)
{
    static const uint16_t cbTag = 4;
    static const uint16_t cbChunkType = 4;
    static const uint16_t cbChunkSize = 4;

    uint32_t cbFormatChunk = sizeof(WAVEFORMAT) + pformat->cbSize;
    uint32_t cbRiffChunk = 0;       // NOTE: This isn't technically accurate for a RIFF/WAV file, but it's fine for Truman/Newman/Skyman
    uint32_t cbDataChunk = 0;       // NOTE: Similarly, this isn't technically correct for the 'data' chunk, but it's fine for Truman/Newman/Skyman

    size_t cbHeader =
        cbTag + cbChunkSize +       // 'RIFF' #size_of_RIFF#
        cbChunkType +               // 'WAVE'
        cbChunkType + cbChunkSize + // 'fmt ' #size_fmt#
        cbFormatChunk +             // actual format
        cbChunkType + cbChunkSize;  // 'data' #size_of_data#

    // Allocate the buffer, and create a ptr we'll use to advance thru the buffer as we're writing stuff into it
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[cbHeader]);
    auto ptr = buffer.get();

    // The 'RIFF' header (consists of 'RIFF' followed by size of payload that follows)
    ptr = FormatBufferWriteChars(ptr, "RIFF", cbTag);
    ptr = FormatBufferWriteNumber(ptr, cbRiffChunk);

    // The 'WAVE' chunk header
    ptr = FormatBufferWriteChars(ptr, "WAVE", cbChunkType);

    // The 'fmt ' chunk (consists of 'fmt ' followed by the total size of the WAVEFORMAT(EX)(TENSIBLE), followed by the WAVEFORMAT(EX)(TENSIBLE)
    ptr = FormatBufferWriteChars(ptr, "fmt ", cbChunkType);
    ptr = FormatBufferWriteNumber(ptr, cbFormatChunk);
    ptr = FormatBufferWriteBytes(ptr, (uint8_t*)pformat, cbFormatChunk);

    // The 'data' chunk is next
    ptr = FormatBufferWriteChars(ptr, "data", cbChunkType);
    ptr = FormatBufferWriteNumber(ptr, cbDataChunk);

    // Now that we've prepared the header/buffer, send it along to Truman/Newman/Skyman via UspWrite
    SPX_DBG_ASSERT(cbHeader == size_t(ptr - buffer.get()));
    UspWrite(buffer.get(), cbHeader);
}

void CSpxUspRecoEngineAdapter::UspWrite(const uint8_t* buffer, size_t byteToWrite)
{
    SPX_DBG_TRACE_VERBOSE_IF(byteToWrite == 0, "%s(..., %d)", __FUNCTION__, byteToWrite);

    auto fn = !m_fUseBufferedImplementation || m_servicePreferedBufferSizeSendingNow == 0
        ? &CSpxUspRecoEngineAdapter::UspWrite_Actual
        : &CSpxUspRecoEngineAdapter::UspWrite_Buffered;

    (this->*fn)(buffer, byteToWrite);
}

void CSpxUspRecoEngineAdapter::UspWrite_Actual(const uint8_t* buffer, size_t byteToWrite)
{
    SPX_DBG_ASSERT(m_handle != nullptr || IsState(UspState::Terminating) || IsState(UspState::Zombie));
    if (!IsState(UspState::Terminating) && !IsState(UspState::Zombie) && m_handle != nullptr)
    {
        SPX_DBG_TRACE_VERBOSE("%s(..., %d)", __FUNCTION__, byteToWrite);
        m_handle->WriteAudio(buffer, byteToWrite);
    }
}

void CSpxUspRecoEngineAdapter::UspWrite_Buffered(const uint8_t* buffer, size_t bytesToWrite)
{
    bool flushBuffer = bytesToWrite == 0;

    if (m_buffer.get() == nullptr)
    {
        m_buffer = SpxAllocSharedUint8Buffer(m_servicePreferedBufferSizeSendingNow);
        m_bytesInBuffer = m_servicePreferedBufferSizeSendingNow;

        m_ptrIntoBuffer = m_buffer.get();
        m_bytesLeftInBuffer = m_bytesInBuffer;
    }

    for (;;)
    {
        if (flushBuffer || (m_bytesInBuffer > 0 && m_bytesLeftInBuffer == 0))
        {
            auto bytesToFlush = m_bytesInBuffer - m_bytesLeftInBuffer;
            UspWrite_Actual(m_buffer.get(), bytesToFlush);

            m_bytesLeftInBuffer = m_bytesInBuffer;
            m_ptrIntoBuffer = m_buffer.get();
        }

        if (flushBuffer)
        {
            m_buffer = nullptr;
            m_bytesInBuffer = 0;
            m_ptrIntoBuffer = nullptr;
            m_bytesLeftInBuffer = 0;
        }

        if (bytesToWrite == 0)
        {
            break;
        }

        size_t bytesThisLoop = std::min(bytesToWrite, m_bytesLeftInBuffer);
        std::memcpy(m_ptrIntoBuffer, buffer, bytesThisLoop);

        m_ptrIntoBuffer += bytesThisLoop;
        m_bytesLeftInBuffer -= bytesThisLoop;
        bytesToWrite -= bytesThisLoop;
    }
}

void CSpxUspRecoEngineAdapter::UspWrite_Flush()
{
    SPX_DBG_ASSERT(m_handle != nullptr || IsState(UspState::Terminating) || IsState(UspState::Zombie));
    if (!IsState(UspState::Terminating) && !IsState(UspState::Zombie) && m_handle != nullptr)
    {
        UspWrite_Buffered(nullptr, 0);
        m_handle->FlushAudio();
    }
}

void CSpxUspRecoEngineAdapter::OnSpeechStartDetected(const USP::SpeechStartDetectedMsg& message)
{
    // The USP message for SpeechStartDetected isn't what it might sound like in all "reco modes" ... 
    // * In INTERACTIVE mode, it works as it sounds. It indicates the begging of speech for the "phrase" message that will arrive later
    // * In CONTINUOUS modes, however, it corresponds to the time of the beginning of speech for the FIRST "phrase" message of many inside one turn

    SPX_DBG_TRACE_VERBOSE("Response: Speech.StartDetected message. Speech starts at offset %" PRIu64 " (100ns).\n", message.offset);

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (IsState(UspState::WaitingForPhrase))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterDetectedSpeechStart()", __FUNCTION__);
        SPX_DBG_ASSERT(GetSite() != nullptr);
        GetSite()->AdapterDetectedSpeechStart(this, message.offset);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnSpeechEndDetected(const USP::SpeechEndDetectedMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Speech.EndDetected message. Speech ends at offset %" PRIu64 " (100ns)\n", message.offset);

    WriteLock_Type writeLock(m_stateMutex);
    auto requestIdle = m_singleShot && ChangeState(AudioState::Sending, AudioState::Stopping);

    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (IsStateBetweenIncluding(UspState::WaitingForPhrase, UspState::WaitingForTurnEnd) &&
             (IsState(AudioState::Idle) ||
              IsState(AudioState::Sending) ||
              IsState(AudioState::Stopping)))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterDetectedSpeechEnd()", __FUNCTION__);
        SPX_DBG_ASSERT(GetSite());
        GetSite()->AdapterDetectedSpeechEnd(this, message.offset);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }

    SPX_DBG_TRACE_VERBOSE("%s: Flush ... (audioState/uspState=%d/%d)  USP-FLUSH", __FUNCTION__, m_audioState, m_uspState);
    UspWrite_Flush();

    if (requestIdle && !IsBadState())
    {
        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterRequestingAudioIdle() ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
        SPX_DBG_ASSERT(GetSite());
        GetSite()->AdapterRequestingAudioIdle(this);
    }
}

void CSpxUspRecoEngineAdapter::OnSpeechHypothesis(const USP::SpeechHypothesisMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Speech.Hypothesis message. Starts at offset %" PRIu64 ", with duration %" PRIu64 " (100ns). Text: %ls\n", message.offset, message.duration, message.text.c_str());

    ReadLock_Type readLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(readLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (IsState(UspState::WaitingForPhrase))
    {
        readLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->FireAdapterResult_Intermediate()", __FUNCTION__);

        SPX_DBG_ASSERT(GetSite());
        auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());

        auto result = factory->CreateIntermediateResult(nullptr, message.text.c_str(), ResultType::Speech);
        auto namedProperties = SpxQueryInterface<ISpxNamedProperties>(result);
        namedProperties->SetStringValue(g_RESULT_Json, message.json.c_str());

        GetSite()->FireAdapterResult_Intermediate(this, message.offset, result);
    }
    else
    {
        SPX_DBG_ASSERT(readLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnSpeechFragment(const USP::SpeechFragmentMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Speech.Fragment message. Starts at offset %" PRIu64 ", with duration %" PRIu64 " (100ns). Text: %ls\n", message.offset, message.duration, message.text.c_str());
    SPX_DBG_ASSERT(!IsInteractiveMode());

    bool sendIntermediate = false;

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (ChangeState(UspState::WaitingForIntent, UspState::WaitingForIntent2))
    {
        SPX_DBG_TRACE_VERBOSE("%s: Intent never came from service!!", __FUNCTION__);
        sendIntermediate = true;

        writeLock.unlock();
        FireFinalResultLater_WaitingForIntentComplete();

        writeLock.lock();
        ChangeState(UspState::WaitingForIntent2, UspState::WaitingForPhrase);
    }
    else if (IsState(UspState::WaitingForPhrase))
    {
        sendIntermediate = true;
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }

    if (sendIntermediate)
    {
        writeLock.unlock(); // calls to site shouldn't hold locks
        SPX_DBG_TRACE_VERBOSE("%s: site->FireAdapterResult_Intermediate()", __FUNCTION__);

        SPX_DBG_ASSERT(GetSite());
        auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
        auto result = factory->CreateIntermediateResult(nullptr, message.text.c_str(), ResultType::Speech);
        auto namedProperties = SpxQueryInterface<ISpxNamedProperties>(result);
        namedProperties->SetStringValue(g_RESULT_Json, message.json.c_str());

        GetSite()->FireAdapterResult_Intermediate(this, message.offset, result);
    }
}

void CSpxUspRecoEngineAdapter::OnSpeechPhrase(const USP::SpeechPhraseMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Speech.Phrase message. Status: %d, Text: %ls, starts at %" PRIu64 ", with duration %" PRIu64 " (100ns).\n", message.recognitionStatus, message.displayText.c_str(), message.offset, message.duration);

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (m_expectIntentResponse && 
             message.recognitionStatus == USP::RecognitionStatus::Success &&
             ChangeState(UspState::WaitingForPhrase, UspState::WaitingForIntent))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: FireFinalResultLater()", __FUNCTION__);
        FireFinalResultLater(message);
    }
    else if (( IsInteractiveMode() && ChangeState(UspState::WaitingForPhrase, UspState::WaitingForTurnEnd)) ||
             (!IsInteractiveMode() && ChangeState(UspState::WaitingForPhrase, UspState::WaitingForPhrase)))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: FireFinalResultNow()", __FUNCTION__);
        FireFinalResultNow(message);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for warning trace
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnTranslationHypothesis(const USP::TranslationHypothesisMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Translation.Hypothesis message. RecoText: %ls, TranslationStatus: %d, starts at %" PRIu64 ", with duration %" PRIu64 " (100ns).\n",
        message.text.c_str(), message.translation.translationStatus, message.offset, message.duration);
    auto resultMap = message.translation.translations;
#ifdef _DEBUG
    for (const auto& it : resultMap)
    {
        SPX_DBG_TRACE_VERBOSE("          Translation in %ls: %ls,\n", it.first.c_str(), it.second.c_str());
    }
#endif

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace warning
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING (Err/Terminating/Zombie)... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
    else if (IsState(UspState::WaitingForPhrase))
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace statement
        {
            writeLock.unlock();
            SPX_DBG_TRACE_SCOPE("Fire final translation result: Creating Result", "FireFinalResul: GetSite()->FireAdapterResult_FinalResult()  complete!");

            // Create the result
            auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
            auto result = factory->CreateIntermediateResult(nullptr, message.text.c_str(), ResultType::TranslationText);

            auto namedProperties = SpxQueryInterface<ISpxNamedProperties>(result);
            namedProperties->SetStringValue(g_RESULT_Json, message.json.c_str());

            // Update our result to be an "TranslationText" result.
            auto initTranslationResult = SpxQueryInterface<ISpxTranslationTextResultInit>(result);
            TranslationTextStatus status;
            switch (message.translation.translationStatus)
            {
            case ::USP::TranslationStatus::Success:
                status = TranslationTextStatus::Success;
                break;
            case ::USP::TranslationStatus::Error:
                status = TranslationTextStatus::Error;
                break;
            default:
                status = TranslationTextStatus::Error;
                SPX_THROW_HR(SPXERR_RUNTIME_ERROR);
                break;
            }

            initTranslationResult->InitTranslationTextResult(status, message.translation.translations, message.translation.failureReason);

            // Fire the result
            SPX_ASSERT(GetSite() != nullptr);
            GetSite()->FireAdapterResult_Intermediate(this, message.offset, result);
        }
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for warning trace
        SPX_TRACE_WARNING("%s: Unexpected USP State transition (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }

}

void CSpxUspRecoEngineAdapter::OnTranslationPhrase(const USP::TranslationPhraseMsg& message)
{
    auto resultMap = message.translation.translations;

    SPX_DBG_TRACE_VERBOSE("Response: Translation.Phrase message. RecoStatus: %d, TranslationStatus: %d, RecoText: %ls, starts at %" PRIu64 ", with duration %" PRIu64 " (100ns).\n",
        message.recognitionStatus, message.translation.translationStatus,
        message.text.c_str(), message.offset, message.duration);
#ifdef _DEBUG
    if (message.translation.translationStatus != ::USP::TranslationStatus::Success)
    {
        SPX_DBG_TRACE_VERBOSE(" FailureReason: %ls.", message.translation.failureReason.c_str());
    }
    for (const auto& it : resultMap)
    {
        SPX_DBG_TRACE_VERBOSE("          , tranlated to %ls: %ls,\n", it.first.c_str(), it.second.c_str());
    }
#endif

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace warning
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING (Err/Terminating/Zombie)... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
    else if ((IsInteractiveMode() && ChangeState(UspState::WaitingForPhrase, UspState::WaitingForTurnEnd)) ||
        (!IsInteractiveMode() && ChangeState(UspState::WaitingForPhrase, UspState::WaitingForPhrase)))
    {
        SPX_DBG_TRACE_SCOPE("Fire final translation result: Creating Result", "FireFinalResul: GetSite()->FireAdapterResult_FinalResult()  complete!");

        writeLock.unlock(); // calls to site shouldn't hold locks

        // Create the result
        auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
        auto result = factory->CreateFinalResult(nullptr, message.text.c_str(), ResultType::TranslationText);

        auto namedProperties = SpxQueryInterface<ISpxNamedProperties>(result);
        namedProperties->SetStringValue(g_RESULT_Json, message.json.c_str());

        // Update our result to be an "TranslationText" result.
        auto initTranslationResult = SpxQueryInterface<ISpxTranslationTextResultInit>(result);
        // Todo: better convert translation status
        TranslationTextStatus status;
        switch (message.translation.translationStatus)
        {
        case ::USP::TranslationStatus::Success:
            status = TranslationTextStatus::Success;
            break;
        case ::USP::TranslationStatus::Error:
            status = TranslationTextStatus::Error;
            break;
        default:
            status = TranslationTextStatus::Error;
            SPX_THROW_HR(SPXERR_RUNTIME_ERROR);
            break;
        }
        initTranslationResult->InitTranslationTextResult(status, message.translation.translations, message.translation.failureReason);

        // Fire the result
        SPX_ASSERT(GetSite() != nullptr);
        GetSite()->FireAdapterResult_FinalResult(this, message.offset, result);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for warning trace
        SPX_TRACE_WARNING("%s: Unexpected USP State transition (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnTranslationSynthesis(const USP::TranslationSynthesisMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Translation.Synthesis message. Audio data size: \n", message.audioLength);
    SPX_DBG_ASSERT(GetSite());

    // TODO: RobCh: Do something with the other fields in UspMsgSpeechPhrase
    auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
    auto result = factory->CreateFinalResult(nullptr, L"", ResultType::TranslationSynthesis);

    // Update our result to be an "TranslationSynthesis" result.
    auto initTranslationResult = SpxQueryInterface<ISpxTranslationSynthesisResultInit>(result);
    initTranslationResult->InitTranslationSynthesisResult(TranslationSynthesisStatus::Success, message.audioBuffer, message.audioLength, L"");

    // Fire the result
    SPX_ASSERT(GetSite() != nullptr);
    GetSite()->FireAdapterResult_TranslationSynthesis(this, result);
}

void CSpxUspRecoEngineAdapter::OnTranslationSynthesisEnd(const USP::TranslationSynthesisEndMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Translation.Synthesis.End message. Status: %d, Reason: %ls\n", (int)message.synthesisStatus, message.failureReason.c_str());
    SPX_DBG_ASSERT(GetSite());

    auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
    auto result = factory->CreateFinalResult(nullptr, L"", ResultType::TranslationSynthesis);

    // Update our result to be an "TranslationSynthesis" result.
    auto initTranslationResult = SpxQueryInterface<ISpxTranslationSynthesisResultInit>(result);
    TranslationSynthesisStatus status;
    switch (message.synthesisStatus)
    {
    case ::USP::SynthesisStatus::Success:
        // Indicates the end of syntheis.
        status = TranslationSynthesisStatus::SynthesisEnd;
        break;
    case ::USP::SynthesisStatus::Error:
        status = TranslationSynthesisStatus::Error;
        break;
    default:
        status = TranslationSynthesisStatus::Error;
        SPX_THROW_HR(SPXERR_RUNTIME_ERROR);
        break;
    }
    initTranslationResult->InitTranslationSynthesisResult(status, nullptr, 0, message.failureReason);

    // Fire the result
    SPX_ASSERT(GetSite() != nullptr);
    GetSite()->FireAdapterResult_TranslationSynthesis(this, result);
}

void CSpxUspRecoEngineAdapter::OnTurnStart(const USP::TurnStartMsg& message)
{
    SPX_DBG_TRACE_VERBOSE("Response: Turn.Start message. Context.ServiceTag: %s\n", message.contextServiceTag.c_str());

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (ChangeState(UspState::WaitingForTurnStart, UspState::WaitingForPhrase))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterStartedTurn()", __FUNCTION__);
        SPX_DBG_ASSERT(GetSite());
        GetSite()->AdapterStartedTurn(this, message.contextServiceTag);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnTurnEnd(const USP::TurnEndMsg& message)
{
    SPX_DBG_TRACE_SCOPE("CSpxUspRecoEngineAdapter::OnTurnEnd ... started... ", "CSpxUspRecoEngineAdapter::OnTurnEnd ... DONE!");
    SPX_DBG_TRACE_VERBOSE("Response: Turn.End message.\n");
    UNUSED(message);

    WriteLock_Type writeLock(m_stateMutex);
    auto prepareReady = !m_singleShot && ChangeState(AudioState::Sending, AudioState::Ready);
    auto requestIdle = m_singleShot && ChangeState(AudioState::Sending, AudioState::Stopping);
    auto adapterTurnStopped = false;

    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (( IsInteractiveMode() && ChangeState(UspState::WaitingForTurnEnd, UspState::Idle)) ||
             (!IsInteractiveMode() && ChangeState(UspState::WaitingForPhrase, UspState::Idle)))
    {
        adapterTurnStopped = true;
    }
    else if (ChangeState(UspState::WaitingForIntent, UspState::WaitingForIntent2))
    {
        SPX_DBG_TRACE_VERBOSE("%s: Intent never came from service!!", __FUNCTION__);
        adapterTurnStopped = true;

        writeLock.unlock();
        FireFinalResultLater_WaitingForIntentComplete();

        writeLock.lock();
        ChangeState(UspState::WaitingForIntent2, UspState::Idle);
    }
    else
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }

    if (prepareReady && !IsBadState()) 
    {
        SPX_DBG_TRACE_VERBOSE("%s: PrepareAudioReadyState()", __FUNCTION__);
        PrepareAudioReadyState();
    }

    writeLock.unlock(); // calls to site shouldn't hold locks

    if (adapterTurnStopped)
    {
        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterStoppedTurn()", __FUNCTION__);
        SPX_DBG_ASSERT(GetSite());
        GetSite()->AdapterStoppedTurn(this);
    }

    if (requestIdle)
    {
        SPX_DBG_TRACE_VERBOSE("%s: UspWrite_Flush()  USP-FLUSH", __FUNCTION__);
        UspWrite_Flush();

        SPX_DBG_TRACE_VERBOSE("%s: site->AdapterRequestingAudioIdle() ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
        SPX_DBG_ASSERT(GetSite());
        GetSite()->AdapterRequestingAudioIdle(this);
    }
}

void CSpxUspRecoEngineAdapter::OnError(const std::string& error)
{
    SPX_DBG_TRACE_VERBOSE("Response: On Error: %s.\n", error.c_str());

    WriteLock_Type writeLock(m_stateMutex);
    if (IsBadState())
    {
        SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
        SPX_DBG_TRACE_VERBOSE("%s: IGNORING... (audioState/uspState=%d/%d) %s", __FUNCTION__, m_audioState, m_uspState, IsState(UspState::Terminating) ? "(USP-TERMINATING)" : "********** USP-UNEXPECTED !!!!!!");
    }
    else if (ShouldResetAfterError() && ChangeState(AudioState::Ready, UspState::Idle))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: ResetAfterError!! ... error='%s'", __FUNCTION__, error.c_str());

        SPX_DBG_ASSERT(GetSite() != nullptr);
        GetSite()->Error(this, error);

        ResetAfterError();
    }
    else if (ChangeState(UspState::Error))
    {
        writeLock.unlock(); // calls to site shouldn't hold locks

        SPX_DBG_TRACE_VERBOSE("%s: site->Error() ... error='%s'", __FUNCTION__, error.c_str());
        SPX_DBG_ASSERT(GetSite() != nullptr);
        GetSite()->Error(this, error);
    }
    else
    {
       SPX_DBG_ASSERT(writeLock.owns_lock()); // need to keep the lock for trace message
       SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
    }
}

void CSpxUspRecoEngineAdapter::OnUserMessage(const std::string& path, const std::string& contentType, const uint8_t* buffer, size_t size)
{
    UNUSED(contentType);
    SPX_DBG_TRACE_VERBOSE("Response: Usp User Message: %s, content-type=%s", path.c_str(), contentType.c_str());

    if (path == "response")
    {
        ReadLock_Type readLock(m_stateMutex);
        if (IsState(UspState::WaitingForIntent))
        {
            readLock.unlock(); // calls to site shouldn't hold locks

            std::string luisJson((const char*)buffer, size);
            SPX_DBG_TRACE_VERBOSE("USP User Message: response; luisJson='%s'", luisJson.c_str());
            FireFinalResultLater_WaitingForIntentComplete(luisJson);
        }
        else
        {
            SPX_DBG_ASSERT(readLock.owns_lock()); // need to keep the lock for trace message
            SPX_DBG_TRACE_WARNING("%s: UNEXPECTED USP State transition ... (audioState/uspState=%d/%d)", __FUNCTION__, m_audioState, m_uspState);
        }
    }
}

uint8_t* CSpxUspRecoEngineAdapter::FormatBufferWriteBytes(uint8_t* buffer, const uint8_t* source, size_t bytes)
{
    std::memcpy(buffer, source, bytes);
    return buffer + bytes;
}

uint8_t* CSpxUspRecoEngineAdapter::FormatBufferWriteNumber(uint8_t* buffer, uint32_t number)
{
    std::memcpy(buffer, &number, sizeof(number));
    return buffer + sizeof(number);
}

uint8_t* CSpxUspRecoEngineAdapter::FormatBufferWriteChars(uint8_t* buffer, const char* psz, size_t cch)
{
    std::memcpy(buffer, psz, cch);
    return buffer + cch;
}

std::list<std::string> CSpxUspRecoEngineAdapter::GetListenForListFromSite()
{
    SPX_DBG_ASSERT(GetSite() != nullptr);
    return GetSite()->GetListenForList();
}

std::string CSpxUspRecoEngineAdapter::GetDgiJsonFromListenForList(std::list<std::string>& listenForList)
{
    SPX_DBG_ASSERT(GetSite() != nullptr);
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    auto noDGI = properties->GetBooleanValue(L"CARBON-INTERNAL-USP-NoDGI", false);

    std::string dgiJson;

    std::list<std::string> grammars;
    std::list<std::string> genericItems;

    for (auto listenFor : listenForList)
    {
        if (listenFor.length() > 3 && 
            listenFor[0] == '{' && listenFor[listenFor.length() - 1] == '}' && 
            listenFor.find(':') != std::string::npos)
        {
            std::string ref = listenFor.substr(1, listenFor.length() - 2);
            ref.replace(ref.find(':'), 1, "/");
            grammars.push_back(std::move(ref));
        }
        else
        {
            genericItems.push_back(std::move(listenFor));
        }
    }

    if (grammars.size() > 0 || genericItems.size() > 0)
    {
        bool appendComma = false;

        dgiJson = "{";  // start object

        if (genericItems.size() > 0)
        {
            dgiJson += R"("Groups": [)";  // start "Group" array
            dgiJson += R"({"Type":"Generic","Items":[)"; // start Generic Items array

            appendComma = false;
            for (auto item : genericItems)
            {
                dgiJson += appendComma ? "," : "";
                dgiJson += R"({"Text":")";
                dgiJson += item;
                dgiJson += R"("})";
                appendComma = true;
            }

            dgiJson += "]}"; // close Generic Items array
            dgiJson += "]";  // close "Group" array

            appendComma = true;
        }

        if (grammars.size() > 0)
        {
            dgiJson += appendComma ? "," : "";
            dgiJson += R"("ReferenceGrammars": [)";

            appendComma = false;
            for (auto grammar : grammars)
            {
                // deal with commas
                dgiJson += appendComma ? "," : "";
                dgiJson += "\"";
                dgiJson += grammar;
                dgiJson += "\"";
                appendComma = true;
            }

            dgiJson += "]";
            appendComma = true;
        }

        dgiJson += "}";  // close object
    }

    return noDGI ? "" : dgiJson;
}

void CSpxUspRecoEngineAdapter::GetIntentInfoFromSite(std::string& provider, std::string& id, std::string& key)
{
    SPX_DBG_ASSERT(GetSite() != nullptr);
    GetSite()->GetIntentInfo(provider, id, key);
}

std::string CSpxUspRecoEngineAdapter::GetLanguageUnderstandingJsonFromIntentInfo(const std::string& provider, const std::string& id, const std::string& key)
{
    SPX_DBG_ASSERT(GetSite() != nullptr);
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    auto noIntentJson = properties->GetBooleanValue(L"CARBON-INTERNAL-USP-NoIntentJson", false);

    std::string intentJson;
    if (!provider.empty() && !id.empty() && !key.empty())
    {
        intentJson = "{"; // start object

        intentJson += R"("provider":")";
        intentJson += provider + R"(",)";

        intentJson += R"("id":")";
        intentJson += id + R"(",)";

        intentJson += R"("key":")";
        intentJson += key + R"(")";

        intentJson += "}"; // end object
    }

    return noIntentJson ? "" : intentJson;
}

std::string CSpxUspRecoEngineAdapter::GetSpeechContextJson(const std::string& dgiJson, const std::string& intentJson)
{
    std::string contextJson;

    if (!dgiJson.empty() || !intentJson.empty())
    {
        bool appendComma = false;
        contextJson += "{";

        if (!dgiJson.empty())
        {
            contextJson += appendComma ? "," : "";
            contextJson += R"("dgi":)";
            contextJson += dgiJson;
            appendComma = true;
        }

        if (!intentJson.empty())
        {
            contextJson += appendComma ? "," : "";
            contextJson += R"("intent":)";
            contextJson += intentJson;
            appendComma = true;
        }

        contextJson += "}";
    }

    return contextJson;
}


void CSpxUspRecoEngineAdapter::FireFinalResultLater(const USP::SpeechPhraseMsg& message)
{
    m_finalResultMessageToFireLater = message;
}

void CSpxUspRecoEngineAdapter::FireFinalResultNow(const USP::SpeechPhraseMsg& message, const std::string& luisJson)
{
    SPX_DBG_TRACE_SCOPE("FireFinalResultNow: Creating Result", "FireFinalResultNow: GetSite()->FireAdapterResult_FinalResult()  complete!");

    // Create the result
    SPX_DBG_ASSERT(GetSite() != nullptr);
    auto factory = SpxQueryService<ISpxRecoResultFactory>(GetSite());
    auto result = factory->CreateFinalResult(nullptr, message.displayText.c_str(), ResultType::Speech);

    auto namedProperties = SpxQueryInterface<ISpxNamedProperties>(result);
    namedProperties->SetStringValue(g_RESULT_Json, message.json.c_str());

    // Do we already have the LUIS json payload from the service (1-hop)
    if (!luisJson.empty())
    {
        namedProperties->SetStringValue(g_RESULT_LanguageUnderstandingJson, PAL::ToWString(luisJson).c_str());
    }

    SPX_ASSERT(GetSite() != nullptr);
    GetSite()->FireAdapterResult_FinalResult(this, message.offset, result);
}

void CSpxUspRecoEngineAdapter::FireFinalResultLater_WaitingForIntentComplete(const std::string& luisJson)
{
    SPX_DBG_ASSERT(m_expectIntentResponse);
    FireFinalResultNow(m_finalResultMessageToFireLater, luisJson);
    m_finalResultMessageToFireLater = USP::SpeechPhraseMsg();
}

bool CSpxUspRecoEngineAdapter::ChangeState(AudioState fromAudioState, UspState fromUspState, AudioState toAudioState, UspState toUspState)
{
    if (fromAudioState == m_audioState &&       // are we in correct audio state, and ...
        fromUspState == m_uspState &&           // are we in correct usp state? ... if so great! but ...
        ((fromUspState != UspState::Error &&        // don't allow transit from Error
          fromUspState != UspState::Zombie &&       // don't allow transit from Zombie
          fromUspState != UspState::Terminating) || // don't allow transit from Terminating ...
         ((fromUspState == toUspState) ||           // unless we're staying in that same usp state
          (fromUspState == UspState::Error &&           // or we're going from Error to Terminating
           toUspState == UspState::Terminating) ||
          (fromUspState == UspState::Terminating &&     // or we're going from Terminating to Zombie
           toUspState == UspState::Zombie))))
    {
        SPX_DBG_TRACE_VERBOSE("%s; audioState/uspState: %d/%d => %d/%d %s%s%s%s%s", __FUNCTION__, 
            fromAudioState, fromUspState,
            toAudioState, toUspState,
            toUspState == UspState::Error ? "USP-ERRORERROR" : "",
            (fromAudioState == AudioState::Idle && fromUspState == UspState::Idle &&
             toAudioState == AudioState::Ready && toUspState == UspState::Idle) ? "USP-START" : "",
            (toAudioState == AudioState::Idle && toUspState == UspState::Idle) ? "USP-DONE" : "",
            toUspState == UspState::Terminating ? "USP-TERMINATING" : "",
            toUspState == UspState::Zombie ? "USP-ZOMBIE" : ""
            );

        m_audioState = toAudioState;
        m_uspState = toUspState;
        return true;
    }

    return false;
}

void CSpxUspRecoEngineAdapter::PrepareFirstAudioReadyState(WAVEFORMATEX* format)
{
    SPX_DBG_ASSERT(IsState(AudioState::Ready, UspState::Idle));

    auto sizeOfFormat = sizeof(WAVEFORMATEX) + format->cbSize;
    m_format = SpxAllocWAVEFORMATEX(sizeOfFormat);
    memcpy(m_format.get(), format, sizeOfFormat);

    PrepareAudioReadyState();
}

void CSpxUspRecoEngineAdapter::PrepareAudioReadyState()
{
    SPX_DBG_ASSERT(IsState(AudioState::Ready, UspState::Idle));

    m_servicePreferedBufferSizeSendingNow = 0;
    EnsureUspInit();
}

void CSpxUspRecoEngineAdapter::SendPreAudioMessages()
{
    SPX_DBG_ASSERT(IsState(AudioState::Sending));

    UspSendSpeechContext();
    UspWriteFormat(m_format.get());
    m_servicePreferedBufferSizeSendingNow = (size_t)m_format->nSamplesPerSec * m_format->nBlockAlign * m_servicePreferedMilliseconds / 1000;
}

bool CSpxUspRecoEngineAdapter::ShouldResetAfterError()
{
    SPX_DBG_ASSERT(GetSite() != nullptr);
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    SPX_IFTRUE_THROW_HR(properties == nullptr, SPXERR_UNEXPECTED_USP_SITE_FAILURE);
    return properties->GetBooleanValue(L"CARBON-INTERNAL-USP-ResetAfterError") && m_format != nullptr;
}

void CSpxUspRecoEngineAdapter::ResetAfterError()
{
    SPX_DBG_ASSERT(ShouldResetAfterError());
    m_handle.reset();

    PrepareAudioReadyState();
}


} } } } // Microsoft::CognitiveServices::Speech::Impl
