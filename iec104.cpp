/*
 * Fledge IEC 104 north plugin.
 *
 * Copyright (c) 2020, RTE (https://www.rte-france.com)
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Akli Rahmoun <akli.rahmoun at rte-france.com>
 */
#include <iec104.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <plugin_api.h>
#include <config_category.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>


using namespace std;

static bool running = true;

/**
 * Constructor for the IEC104 Server object
 */
IEC104Server::IEC104Server()
{
    m_log = Logger::getLogger();

    m_config = new IEC104Config();

    /* create a new slave/server instance with default connection parameters and
     * default message queue size */
    m_slave = CS104_Slave_create(10, 10);

    m_oper = NULL;

    CS104_Slave_setLocalAddress(m_slave, "0.0.0.0");

    /* Set mode to a single redundancy group
     * NOTE: library has to be compiled with
     * CONFIG_CS104_SUPPORT_SERVER_MODE_SINGLE_REDUNDANCY_GROUP enabled (=1)
     */
    CS104_Slave_setServerMode(m_slave, CS104_MODE_SINGLE_REDUNDANCY_GROUP);

    /* when you have to tweak the APCI parameters (t0-t3, k, w) you can access
     * them here */
    CS104_APCIParameters apciParams =
        CS104_Slave_getConnectionParameters(m_slave);

    m_log->info("APCI parameters:");
    m_log->info("  t0: %i", apciParams->t0);
    m_log->info("  t1: %i", apciParams->t1);
    m_log->info("  t2: %i", apciParams->t2);
    m_log->info("  t3: %i", apciParams->t3);
    m_log->info("  k: %i", apciParams->k);
    m_log->info("  w: %i", apciParams->w);

    /* set the callback handler for the clock synchronization command */
    CS104_Slave_setClockSyncHandler(m_slave, clockSyncHandler, this);

    /* set the callback handler for the interrogation command */
    CS104_Slave_setInterrogationHandler(m_slave, interrogationHandler, this);

    /* set handler for other message types */
    CS104_Slave_setASDUHandler(m_slave, asduHandler, this);

    /* set handler to handle connection requests (optional) */
    CS104_Slave_setConnectionRequestHandler(m_slave, connectionRequestHandler, this);

    /* set handler to track connection events (optional) */
    CS104_Slave_setConnectionEventHandler(m_slave, connectionEventHandler, this);

    CS104_Slave_start(m_slave);
}

/**
 * Destructor for the IEC104 Server object
 */
IEC104Server::~IEC104Server() {}

IEC104DataPoint* IEC104Server::m_getDataPoint(int ca, int ioa, int typeId)
{
    IEC104DataPoint* dp = m_exchangeDefinitions[ca][ioa];

    return dp;
}

void IEC104Server::setJsonConfig(const std::string& stackConfig,
                                 const std::string& dataExchangeConfig,
                                const std::string& tlsConfig)
{
    m_config->importExchangeConfig(dataExchangeConfig);
    m_config->importProtocolConfig(stackConfig);

    m_exchangeDefinitions = *m_config->getExchangeDefinitions();
}

/**
 *
 * @param conf	Fledge configuration category
 */
void IEC104Server::configure(const ConfigCategory* config)
{
    if (config->itemExists("name"))
        m_name = config->getValue("name");
    else
        m_log->error("Missing name in configuration");

    if (config->itemExists("protocol_stack") == false) {
        m_log->error("Missing protocol configuration");
        return;
    }

    if (config->itemExists("exchanged_data") == false) {
        m_log->error("Missing exchange data configuration");
        return;
    }

    const std::string protocolStack = config->getValue("protocol_stack");

    const std::string dataExchange = config->getValue("exchanged_data");

    const std::string tlsConfig = std::string("");

    setJsonConfig(protocolStack, dataExchange, tlsConfig);
}

void IEC104Server::registerControl(int (* operation)(char *operation, int paramCount, char *parameters[], ControlDestination destination, ...))
{
    m_oper = operation;
}

void IEC104Server::m_updateDataPoint(IEC104DataPoint* dp, IEC60870_5_TypeID typeId, DatapointValue* value, CP56Time2a ts, uint8_t quality)
{
    if (value == nullptr)
        return;

    switch (typeId) {
        case M_SP_NA_1:
        case M_SP_TB_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_INTEGER) {
                    dp->m_value.sp.value = (unsigned int)value->toInt();
                }

                dp->m_value.sp.quality = quality;
            }

            break;

        case M_DP_NA_1:
        case M_DP_TB_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_INTEGER) {
                    dp->m_value.dp.value = (unsigned int)value->toInt();
                }

                dp->m_value.dp.quality = quality;
            }

            break;

        case M_ST_NA_1:
        case M_ST_TB_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_INTEGER) {
                    dp->m_value.stepPos.posValue = (int)(value->toInt() & 0x7f);
                    dp->m_value.stepPos.transient = (unsigned int)((value->toInt() & 0x80) != 0);
                }

                dp->m_value.stepPos.quality = quality;
            }
            break;

        case M_ME_NA_1: /* normalized value */
        case M_ME_TD_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_FLOAT) {
                    dp->m_value.mv_normalized.value = (float)value->toDouble();
                }

                dp->m_value.mv_normalized.quality = quality;
            }

            break;
 
        case M_ME_NB_1: /* scaled value */
        case M_ME_TE_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_INTEGER) {
                    dp->m_value.mv_scaled.value = (unsigned int)value->toInt();
                }

                dp->m_value.mv_scaled.quality = quality;
            }

            break;

        case M_ME_NC_1: /* short float value */
        case M_ME_TF_1:
            {
                if (value->getType() == DatapointValue::dataTagType::T_FLOAT) {
                    dp->m_value.mv_short.value = (float)value->toDouble();
                }

                dp->m_value.mv_short.quality = quality;
            }

            break;

    }
}

void IEC104Server::m_enqueueSpontDatapoint(IEC104DataPoint* dp, CS101_CauseOfTransmission cot, IEC60870_5_TypeID typeId)
{
    CS101_ASDU asdu = CS101_ASDU_create(alParams, false, cot, 0, dp->m_ca, false, false);

    if (asdu) {
        InformationObject io = NULL;

        switch (typeId) {

            case M_SP_NA_1:
                {
                    io = (InformationObject)SinglePointInformation_create(NULL, dp->m_ioa, dp->m_value.sp.value, dp->m_value.sp.quality);
                }
                break;

            case M_DP_NA_1:
                {
                    io = (InformationObject)DoublePointInformation_create(NULL, dp->m_ioa, (DoublePointValue)dp->m_value.dp.quality, dp->m_value.dp.quality);
                }
                break;

            case M_ME_NA_1:
                {
                    io = (InformationObject)MeasuredValueNormalized_create(NULL, dp->m_ioa, dp->m_value.mv_normalized.value, dp->m_value.mv_normalized.quality);
                }
                break;

            case M_ME_NB_1:
                {
                    io = (InformationObject)MeasuredValueScaled_create(NULL, dp->m_ioa, dp->m_value.mv_scaled.value, dp->m_value.mv_scaled.quality);
                }
                break;

            case M_ME_NC_1:
                {
                    io = (InformationObject)MeasuredValueShort_create(NULL, dp->m_ioa, dp->m_value.mv_short.value, dp->m_value.mv_short.quality);
                }
                break;           

            default:
                m_log->error("Unsupported type ID %i", typeId);

                break;
        }

        if (io) {
            CS101_ASDU_addInformationObject(asdu, io);

            InformationObject_destroy(io);

            CS104_Slave_enqueueASDU(m_slave, asdu);
        }

        CS101_ASDU_destroy(asdu);
    }


}

static Datapoint* createStringDatapoint(const std::string& dataname,
                                        std::string value)
{
    DatapointValue dp_value = DatapointValue(value);
    return new Datapoint(dataname, dp_value);
}

static Datapoint* createLongDatapoint(const std::string& dataname,
                                        long value)
{
    DatapointValue dp_value = DatapointValue(value);
    return new Datapoint(dataname, dp_value);
}

bool IEC104Server::checkTimestamp(CP56Time2a timestamp)
{
    uint64_t currentTime = Hal_getTimeInMs();

    uint64_t commandTime = CP56Time2a_toMsTimestamp(timestamp);

    int timeDiff;

    if (commandTime > currentTime) {
        timeDiff = commandTime - currentTime;
    }
    else {
        timeDiff = currentTime - commandTime;
    }

    if (timeDiff > 5000) {
        m_log->warn("Command timestamp is out of valid range -> ignore command");

        //TODO in this case no ACT_CON should be sent!

        return false;
    }
    else {
        return true;
    }
 }

bool IEC104Server::forwardCommand(CS101_ASDU asdu, InformationObject command)
{
    if (!m_oper) {
        m_log->error("No operation function available");
        return false;
    }
    else {
        IEC60870_5_TypeID typeId = CS101_ASDU_getTypeID(asdu);

        // parameter[0] = CA
        // parameter[1] = IOA
        // parameter[2] = value
        // parameter[3] = select (optional - not used for setpoints)

        int parameterCount = 2;

        char* s_ca = (char*)std::to_string(CS101_ASDU_getCA(asdu)).c_str();
        char* s_ioa = (char*)std::to_string(InformationObject_getObjectAddress(command)).c_str();
        char* s_val = NULL;
        char* s_select = NULL;

        char* parameters[5];

        parameters[0] = s_ca;
        parameters[1] = s_ioa;

        switch (typeId) {

            case C_SC_NA_1:
                {
                    SingleCommand sc = (SingleCommand)command;

                    s_val = (char*)(SingleCommand_getState(sc) ? "1" : "0");
                    s_select = (char*)(SingleCommand_isSelect(sc) ? "1" : "0");

                    parameters[2] = s_val;
                    parameters[3] = s_select;

                    parameterCount = 4;

                    m_oper((char*)"SingleCommand", 4, parameters, DestinationBroadcast, NULL);
                }
                break;

            case C_SC_TA_1:
                {
                    //TODO command with timestamp -> check validity of timestamp

                    SingleCommandWithCP56Time2a sc = (SingleCommandWithCP56Time2a)command;

                    CP56Time2a timestamp = SingleCommandWithCP56Time2a_getTimestamp(sc);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)(SingleCommand_getState((SingleCommand)sc) ? "1" : "0");
                        s_select = (char*)(SingleCommand_isSelect((SingleCommand)sc) ? "1" : "0");

                        parameters[2] = s_val;
                        parameters[3] = s_select;

                        parameterCount = 4;

                        m_oper((char*)"SingleCommandWithCP56Time2a", 4, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }

                }
                break;

            case C_DC_NA_1:
                {
                    DoubleCommand dc = (DoubleCommand)command;

                    s_val = (char*)std::to_string(DoubleCommand_getState(dc)).c_str();
                    s_select = (char*)(DoubleCommand_isSelect(dc) ? "1" : "0");

                    parameters[2] = s_val;
                    parameters[3] = s_select;

                    parameterCount = 4;

                    m_oper((char*)"DoubleCommand", 4, parameters, DestinationBroadcast, NULL);
                }
                break;

            case C_DC_TA_1:
                {
                    DoubleCommandWithCP56Time2a dc = (DoubleCommandWithCP56Time2a)command;

                    CP56Time2a timestamp = DoubleCommandWithCP56Time2a_getTimestamp(dc);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)std::to_string(DoubleCommand_getState((DoubleCommand)dc)).c_str();
                        s_select = (char*)(DoubleCommand_isSelect((DoubleCommand)dc) ? "1" : "0");

                        parameters[2] = s_val;
                        parameters[3] = s_select;

                        parameterCount = 4;

                        m_oper((char*)"DoubleCommandWithCP56Time2a", 4, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }
                }
                break;

            case C_RC_NA_1:
                {
                    StepCommand rc = (StepCommand)command;

                    s_val = (char*)std::to_string(StepCommand_getState(rc)).c_str();
                    s_select = (char*)(StepCommand_isSelect(rc) ? "1" : "0");

                    parameters[2] = s_val;
                    parameters[3] = s_select;

                    parameterCount = 4;

                    m_oper((char*)"StepCommand", 4, parameters, DestinationBroadcast, NULL);
                }
                break;

             case C_RC_TA_1:
                {
                    StepCommandWithCP56Time2a rc = (StepCommandWithCP56Time2a)command;

                    CP56Time2a timestamp = StepCommandWithCP56Time2a_getTimestamp(rc);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)std::to_string(StepCommand_getState((StepCommand)rc)).c_str();
                        s_select = (char*)(StepCommand_isSelect((StepCommand)rc) ? "1" : "0");

                        parameters[2] = s_val;
                        parameters[3] = s_select;

                        parameterCount = 4;

                        m_oper((char*)"StepCommandWithCP56Time2a", 4, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }
                }
                break;

            case C_SE_NA_1:
                {
                    SetpointCommandNormalized spn = (SetpointCommandNormalized)command;

                    s_val = (char*)(std::to_string(SetpointCommandNormalized_getValue(spn)).c_str());

                    parameters[2] = s_val;

                    parameterCount = 3;

                    m_oper((char*)"SetpointNormalized", 3, parameters, DestinationBroadcast, NULL);
                }   
                break;

            case C_SE_TA_1:
                {
                    SetpointCommandNormalizedWithCP56Time2a spn = (SetpointCommandNormalizedWithCP56Time2a)command;

                    CP56Time2a timestamp = SetpointCommandNormalizedWithCP56Time2a_getTimestamp(spn);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)(std::to_string(SetpointCommandNormalized_getValue((SetpointCommandNormalized)spn)).c_str());

                        parameters[2] = s_val;

                        parameterCount = 3;

                        m_oper((char*)"SetpointNormalizedWithCP56Time2a", 3, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }
                }
                break;

            case C_SE_NB_1:
                {
                    SetpointCommandScaled sps = (SetpointCommandScaled)command;

                    s_val = (char*)(std::to_string(SetpointCommandScaled_getValue(sps)).c_str());

                    parameters[2] = s_val;

                    parameterCount = 3;

                    m_oper((char*)"SetpointScaled", 3, parameters, DestinationBroadcast, NULL);
                }
                break;

            case C_SE_TB_1:
                {
                    SetpointCommandScaledWithCP56Time2a sps = (SetpointCommandScaledWithCP56Time2a)command;


                    CP56Time2a timestamp = SetpointCommandScaledWithCP56Time2a_getTimestamp(sps);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)(std::to_string(SetpointCommandScaled_getValue((SetpointCommandScaled)sps)).c_str());

                        parameters[2] = s_val;

                        parameterCount = 3;

                        m_oper((char*)"SetpointScaledWithCP56Time2a", 3, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }
                }
                break;

            case C_SE_NC_1:
                {
                    SetpointCommandShort spf = (SetpointCommandShort)command;

                    s_val = (char*)(std::to_string(SetpointCommandShort_getValue(spf)).c_str());

                    parameters[2] = s_val;

                    parameterCount = 3;

                    m_oper((char*)"SetpointShort", 3, parameters, DestinationBroadcast, NULL);
                }   
                break;

            case C_SE_TC_1:
                {
                    SetpointCommandShortWithCP56Time2a spf = (SetpointCommandShortWithCP56Time2a)command;

                    CP56Time2a timestamp = SetpointCommandShortWithCP56Time2a_getTimestamp(spf);

                    if (checkTimestamp(timestamp)) {
                        s_val = (char*)(std::to_string(SetpointCommandShort_getValue((SetpointCommandShort)spf)).c_str());

                        parameters[2] = s_val;

                        parameterCount = 3;

                        m_oper((char*)"SetpointShortWithCP56Time2a", 3, parameters, DestinationBroadcast, NULL);
                    }
                    else {
                        return false;
                    }
                }
                break;

            default:

                m_log->error("Unsupported command type");

                return false;
        }

        return true;
    }
}

/**
 * Send a block of reading to IEC104 Server
 *
 * @param readings	The readings to send
 * @return 		The number of readings sent
 */
uint32_t IEC104Server::send(const vector<Reading*>& readings)
{
    printf("IEC104Server::send\n");

    if (CS104_Slave_isRunning(m_slave) == false)
    {
        m_log->error("Failed to send data: server not running");
        return 0;
    }

    int16_t value;
    int n = 0;

    for (auto reading = readings.cbegin(); reading != readings.cend();
         reading++)
    {
        vector<Datapoint*>& dataPoints = (*reading)->getReadingData();
        string assetName = (*reading)->getAssetName();

        printf("Reading(asset: %s)\n", assetName.c_str());

        for (Datapoint* dp : dataPoints) {
            printf("  name: %s\n", dp->getName().c_str());

            if (dp->getName() == "data_object") {
              
                int ca = -1;
                int ioa = -1;
                CS101_CauseOfTransmission cot = CS101_COT_UNKNOWN_COT;
                int type = -1;

                DatapointValue dpv = dp->getData();

                vector<Datapoint*>* sdp = dpv.getDpVec();

                bool hasTimestamp = false;
                uint64_t timestamp = 0;
                bool ts_iv = false;
                bool ts_su = false;
                bool ts_sub = false;

                DatapointValue* value = nullptr;

                uint8_t qd = IEC60870_QUALITY_GOOD;

                for (Datapoint* objDp : *sdp) {
                    printf("    attr-name: %s\n", objDp->getName().c_str());

                    DatapointValue attrVal = objDp->getData();

                    if (objDp->getName() == "do_ca") {
                        ca = attrVal.toInt();
                    }
                    else if (objDp->getName() == "do_ioa") {
                        ioa = attrVal.toInt();
                    }
                    else if (objDp->getName() == "do_cot") {
                        cot = (CS101_CauseOfTransmission)attrVal.toInt();
                    }
                    else if (objDp->getName() == "do_type") {
                        type = IEC104DataPoint::getTypeIdFromString(attrVal.toStringValue());
                    }
                    else if (objDp->getName() == "do_value") {
                        value = new DatapointValue(attrVal);
                    }
                    else if (objDp->getName() == "do_quality_iv") {
                        if (attrVal.toInt() != 0)
                            qd |= IEC60870_QUALITY_INVALID;
                    }
                    else if (objDp->getName() == "do_quality_bl") {
                        if (attrVal.toInt() != 0)
                            qd |= IEC60870_QUALITY_BLOCKED;
                    }
                    else if (objDp->getName() == "do_quality_ov") {
                        if (attrVal.toInt() != 0)
                            qd |= IEC60870_QUALITY_OVERFLOW;
                    }
                    else if (objDp->getName() == "do_quality_sb") {
                        if (attrVal.toInt() != 0)
                            qd |= IEC60870_QUALITY_SUBSTITUTED;
                    }
                    else if (objDp->getName() == "do_quality_nt") {
                        if (attrVal.toInt() != 0)
                            qd |= IEC60870_QUALITY_NON_TOPICAL;
                    }
                    else if (objDp->getName() == "do_ts") {
                        timestamp = (uint64_t)attrVal.toInt();
                        hasTimestamp = true;
                    }
                    else if (objDp->getName() == "dp_ts_iv") {
                        if (attrVal.toInt() != 0)
                            ts_iv = true;
                    }
                    else if (objDp->getName() == "dp_ts_su") {
                        if (attrVal.toInt() != 0)
                            ts_su = true;
                    }
                    else if (objDp->getName() == "dp_ts_sub") {
                        if (attrVal.toInt() != 0)
                            ts_sub = true;
                    }
                }

                if (ca != -1 && ioa != -1 && cot != CS101_COT_UNKNOWN_COT && type != -1) {

                    IEC104DataPoint* dp = m_getDataPoint(ca, ioa, 0);

                    if (dp) {

                        CP56Time2a ts = NULL;

                        if (hasTimestamp) {
                            ts = CP56Time2a_createFromMsTimestamp(NULL, timestamp);

                            if (ts) {
                                CP56Time2a_setInvalid(ts, ts_iv);
                                CP56Time2a_setSummerTime(ts, ts_su);
                                CP56Time2a_setSubstituted(ts, ts_sub);
                            }
                        }

                        // update internal value
                        m_updateDataPoint(dp, (IEC60870_5_TypeID)type, value, ts, qd);

                        if (cot == CS101_COT_PERIODIC || cot == CS101_COT_SPONTANEOUS) {
                            m_enqueueSpontDatapoint(dp, cot, (IEC60870_5_TypeID)type);
                        }
                    }
                    else {
                        printf("ERROR: data point %i:%i not found\n", ca, ioa);

                        m_log->error("data point %i:%i not found", ca, ioa);
                    }
                }

                if (value != nullptr) delete value;
            }
            else {
                printf("   --> Unknown data point name\n");
            }
        }

        n++;
    }

    return n;
}

/**
 * Print time in human readable format
 *
 * @param time CP56Time2a time format
 */
void IEC104Server::printCP56Time2a(CP56Time2a time)
{
    Logger::getLogger()->info(
        "%02i:%02i:%02i %02i/%02i/%04i", CP56Time2a_getHour(time),
        CP56Time2a_getMinute(time), CP56Time2a_getSecond(time),
        CP56Time2a_getDayOfMonth(time), CP56Time2a_getMonth(time),
        CP56Time2a_getYear(time) + 2000);
}

/**
 * Callback handler to log sent or received messages (optional)
 *
 * @param parameter
 * @param connection	connection object
 * @param msg	        message
 * @param msgSize	    message size
 * @param sent	        boolean
 */
void IEC104Server::rawMessageHandler(void* parameter,
                                     IMasterConnection connection, uint8_t* msg,
                                     int msgSize, bool sent)
{
    if (sent)
        Logger::getLogger()->info("SEND: ");
    else
        Logger::getLogger()->info("RCVD: ");

    int i;
    for (i = 0; i < msgSize; i++)
    {
        Logger::getLogger()->info("%02x ", msg[i]);
    }
}

/**
 * Callback handler for clock synchronization
 *
 * @param parameter
 * @param connection	connection object
 * @param asdu	        asdu
 * @param newTime	    new time
 * @return 		boolean
 */
bool IEC104Server::clockSyncHandler(void* parameter,
                                    IMasterConnection connection,
                                    CS101_ASDU asdu, CP56Time2a newTime)
{
    Logger::getLogger()->info("Process time sync command with time ");
    printCP56Time2a(newTime);

    uint64_t newSystemTimeInMs = CP56Time2a_toMsTimestamp(newTime);

    /* Set time for ACT_CON message */
    CP56Time2a_setFromMsTimestamp(newTime, Hal_getTimeInMs());

    /* update system time here */

    return true;
}

static bool
isBroadcastCA(int ca, CS101_AppLayerParameters alParams)
{
    if ((alParams->sizeOfCA == 1) && (ca == 0xff))
        return true;

    if ((alParams->sizeOfCA == 2) && (ca == 0xffff))
        return true;

    return false;
}

void IEC104Server::sendInterrogationResponse(IMasterConnection connection, CS101_ASDU asdu, int ca)
{
    CS101_ASDU_setCA(asdu, ca);

    IMasterConnection_sendACT_CON(connection, asdu, false);

    std::map<int, IEC104DataPoint*> ld = m_exchangeDefinitions[ca];

    std::map<int, IEC104DataPoint*>::iterator it;

    sCS101_StaticASDU _asdu;
    uint8_t ioBuf[250];

    CS101_AppLayerParameters alParams =
            IMasterConnection_getApplicationLayerParameters(connection);

    CS101_ASDU newASDU = CS101_ASDU_initializeStatic(&_asdu, alParams, false, CS101_COT_INTERROGATED_BY_STATION, CS101_ASDU_getOA(asdu), ca, false, false);

    for (it = ld.begin(); it != ld.end(); it++)
    {
        IEC104DataPoint* dp = it->second;

        if (dp->isMonitoringType()) {

            printf("  CA: %i IOA: %i type: %i => %s\n", dp->m_ca, dp->m_ioa, dp->m_type, dp->m_label.c_str());

            InformationObject io = NULL;

            //TODO when value not initialized use invalid/non-topical for quality
            //TODO when the value has no original timestamp then create timestamp when sending

            bool sendWithTimestamp = false;

            switch (dp->m_type) {
                case IEC60870_TYPE_SP:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)SinglePointWithCP56Time2a_create((SinglePointWithCP56Time2a)&ioBuf, dp->m_ioa, (bool)(dp->m_value.sp.value), dp->m_value.sp.quality, &cpTs);
                    }
                    else  {
                        io = (InformationObject)SinglePointInformation_create((SinglePointInformation)&ioBuf, dp->m_ioa, (bool)(dp->m_value.sp.value), dp->m_value.sp.quality);
                    }
                    break;

                case IEC60870_TYPE_DP:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)DoublePointWithCP56Time2a_create((DoublePointWithCP56Time2a)&ioBuf, dp->m_ioa, (DoublePointValue)dp->m_value.dp.value, dp->m_value.dp.quality, &cpTs);
                    }
                    else {
                        io = (InformationObject)DoublePointInformation_create((DoublePointInformation)&ioBuf, dp->m_ioa, (DoublePointValue)dp->m_value.dp.value, dp->m_value.dp.quality);
                    }
                    break;

                case IEC60870_TYPE_NORMALIZED:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)MeasuredValueNormalizedWithCP56Time2a_create((MeasuredValueNormalizedWithCP56Time2a)&ioBuf, dp->m_ioa, dp->m_value.mv_normalized.value, dp->m_value.mv_normalized.quality, &cpTs);

                    }
                    else {
                        io = (InformationObject)MeasuredValueNormalized_create((MeasuredValueNormalized)&ioBuf, dp->m_ioa, dp->m_value.mv_normalized.value, dp->m_value.mv_normalized.quality);
                    }
                    break;

                case IEC60870_TYPE_SCALED:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)MeasuredValueScaledWithCP56Time2a_create((MeasuredValueScaledWithCP56Time2a)&ioBuf, dp->m_ioa, dp->m_value.mv_scaled.value, dp->m_value.mv_scaled.quality, &cpTs);
                    }
                    else {
                        io = (InformationObject)MeasuredValueScaled_create((MeasuredValueScaled)&ioBuf, dp->m_ioa, dp->m_value.mv_scaled.value, dp->m_value.mv_scaled.quality);
                    }
                    break;

                case IEC60870_TYPE_SHORT:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)MeasuredValueShortWithCP56Time2a_create((MeasuredValueShortWithCP56Time2a)&ioBuf, dp->m_ioa, dp->m_value.mv_short.value, dp->m_value.mv_short.quality, &cpTs);
                    }
                    else {
                        io = (InformationObject)MeasuredValueShort_create((MeasuredValueShort)&ioBuf, dp->m_ioa, dp->m_value.mv_short.value, dp->m_value.mv_short.quality);
                    }
                    break;

                case IEC60870_TYPE_STEP_POS:
                    if (sendWithTimestamp) {
                        sCP56Time2a cpTs;

                        CP56Time2a_createFromMsTimestamp(&cpTs, Hal_getTimeInMs());

                        io = (InformationObject)StepPositionWithCP56Time2a_create((StepPositionWithCP56Time2a)&ioBuf, dp->m_ioa, dp->m_value.stepPos.posValue, dp->m_value.stepPos.transient, dp->m_value.stepPos.quality, &cpTs);
                    }
                    else {
                        io = (InformationObject)StepPositionInformation_create((StepPositionInformation)&ioBuf, dp->m_ioa, dp->m_value.stepPos.posValue, dp->m_value.stepPos.transient, dp->m_value.stepPos.quality);
                    }
                    break;

            }

            if (io) {
                if (CS101_ASDU_addInformationObject(newASDU, io) == false) {
                    IMasterConnection_sendASDU(connection, newASDU);

                    newASDU = CS101_ASDU_initializeStatic(&_asdu, alParams, false, CS101_COT_INTERROGATED_BY_STATION, CS101_ASDU_getOA(asdu), ca, false, false);

                    CS101_ASDU_addInformationObject(newASDU, io);
                }
            }
        }
    }

    if (newASDU)
        IMasterConnection_sendASDU(connection, newASDU);


    IMasterConnection_sendACT_TERM(connection, asdu);
}

/**
 * Callback handler for station interrogation
 *
 * @param parameter
 * @param connection	connection object
 * @param asdu	        asdu
 * @param qoi	        qoi
 * @return 		boolean
 */
bool IEC104Server::interrogationHandler(void* parameter,
                                        IMasterConnection connection,
                                        CS101_ASDU asdu, uint8_t qoi)
{
    //TODO return quality inalid/non-topical when value has not been initialized -> initialize with this quality flags!

    IEC104Server* self = (IEC104Server*)parameter;

    Logger::getLogger()->info("Received interrogation for group %i", qoi);

    int ca = CS101_ASDU_getCA(asdu);

    CS101_AppLayerParameters alParams =
            IMasterConnection_getApplicationLayerParameters(connection);

    if (qoi != 20) {
        IMasterConnection_sendACT_CON(connection, asdu, true);

        return true;
    }

    if (isBroadcastCA(ca, alParams)) {
        std::map<int, std::map<int, IEC104DataPoint*>>::iterator it;

        for (it = self->m_exchangeDefinitions.begin(); it != self->m_exchangeDefinitions.end(); it++)
        {
            ca = it->first;

            self->sendInterrogationResponse(connection, asdu, ca);
        }
    }
    else {
        if (self->m_exchangeDefinitions.count(ca) == 0) {
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_CA);

            IMasterConnection_sendACT_CON(connection, asdu, true);

            return true;
        }
        else {
            printf("Logical device with CA %i found\n", ca);

            self->sendInterrogationResponse(connection, asdu, ca);
        }
    }

    return true;
}

static bool
isSupportedCommandType(IEC60870_5_TypeID typeId)
{
    if (typeId == C_SC_NA_1) return true;
    if (typeId == C_SC_TA_1) return true;
    if (typeId == C_DC_NA_1) return true;
    if (typeId == C_DC_TA_1) return true;
    if (typeId == C_SE_NA_1) return true;
    if (typeId == C_SE_NB_1) return true;
    if (typeId == C_SE_NC_1) return true;
    if (typeId == C_SE_TA_1) return true;
    if (typeId == C_SE_TB_1) return true;
    if (typeId == C_SE_TC_1) return true;

    return false;
}

bool IEC104Server::checkIfCommandIsConfigured(int ca, int ioa, IEC60870_5_TypeID typeId)
{
    //TODO implement check


    return true;
}

/**
 * Callback handler for ASDU handling
 *
 * @param parameter
 * @param connection	connection object
 * @param asdu	        asdu
 * @return 		boolean
 */
bool IEC104Server::asduHandler(void* parameter, IMasterConnection connection,
                               CS101_ASDU asdu)
{
    IEC104Server* self = (IEC104Server*)parameter;

    if (isSupportedCommandType(CS101_ASDU_getTypeID(asdu)))
    {
        Logger::getLogger()->info("received command");

        if (CS101_ASDU_getCOT(asdu) == CS101_COT_ACTIVATION)
        {
            InformationObject io = CS101_ASDU_getElement(asdu, 0);

            int ca = CS101_ASDU_getCA(asdu);

            std::map<int, IEC104DataPoint*> ld = self->m_exchangeDefinitions[ca];

            if (ld.empty() == false) {
                //TODO check if command has an allowed OA

                int ioa = InformationObject_getObjectAddress(io);       

                IEC104DataPoint* dp = ld[ioa];

                if (dp) {

                    auto typeId = CS101_ASDU_getTypeID(asdu);

                    if (dp->isMatchingCommand(typeId)) {
                        CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION_CON);

                        if (self->forwardCommand(asdu, io) == false) {
                            CS101_ASDU_setNegative(asdu, true);
                        }
                    }
                    else {
                        self->m_log->warn("Unknown command type");
                        CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_TYPE_ID);
                    }
                }
                else {
                    self->m_log->warn("Unknown IOA (%i:%i)", ca, ioa);
                    CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_IOA);
                }
            }
            else {
                self->m_log->warn("Unknown CA: %i", ca);
                CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_CA);
            }

            InformationObject_destroy(io);
        }
        else {
            CS101_ASDU_setCOT(asdu, CS101_COT_UNKNOWN_COT);
        }

        IMasterConnection_sendASDU(connection, asdu);

        return true;
    }

    return false;
}

/**
 * Callback handler for connection request handling
 *
 * @param parameter
 * @param ipAddress	    incoming connection request IP address
 * @return 		boolean
 */
bool IEC104Server::connectionRequestHandler(void* parameter,
                                            const char* ipAddress)
{
    Logger::getLogger()->info("New connection request from %s", ipAddress);

    return true;
}

/**
 * Callback handler for connection event handling
 *
 * @param parameter
 * @param connection	connection object
 * @param event         peer connection event object
 */
void IEC104Server::connectionEventHandler(void* parameter,
                                          IMasterConnection con,
                                          CS104_PeerConnectionEvent event)
{
    if (event == CS104_CON_EVENT_CONNECTION_OPENED)
    {
        Logger::getLogger()->info("Connection opened (%p)", con);
        printf("Connection opened\n");
    }
    else if (event == CS104_CON_EVENT_CONNECTION_CLOSED)
    {
        Logger::getLogger()->info("Connection closed (%p)", con);
        printf("Connection closed\n");
    }
    else if (event == CS104_CON_EVENT_ACTIVATED)
    {
        Logger::getLogger()->info("Connection activated (%p)", con);
    }
    else if (event == CS104_CON_EVENT_DEACTIVATED)
    {
        Logger::getLogger()->info("Connection deactivated (%p)", con);
    }
}

/**
 * Stop the IEC104 Server
 */
void IEC104Server::stop()
{
    if (m_slave)
    {
        CS104_Slave_stop(m_slave);
        CS104_Slave_destroy(m_slave);
    }
}