#include "dynamixel_interface.h"

DynamixelInterface::DynamixelInterface()
{
    this->current_state = DynInterfaceState::INIT;
    this->port_handler = dynamixel::PortHandler::getPortHandler(this->port_path.c_str());
    this->adapter = dynamixel::PacketHandler::getPacketHandler(1.0);
}

void DynamixelInterface::tick()
{
    switch (this->current_state)
    {
        case DynInterfaceState::INIT:
        {
            if (this->port_handler->openPort())
            {
                std::cout << "Successfully opened port" << std::endl;
            }
            else
            {
                std::cerr << "Failed to open port" << std::endl;
                this->errcode = static_cast<int8_t>(DynamixelErrorCodes::PORT_OPEN_FAILED);
                this->current_state = DynInterfaceState::INVALID;
                break;
            }

            if (this->adapter->setBaudRate(this->baud_rate))
            {
                std::cout << "Successfully set baud rate" << std::endl;
            }          
            else
            {
                std::cerr << "failed to set baudrate" << std::endl;
                errcode = static_cast<int8_t>(DynamixelErrorCodes::BAUD_SET_FAILED);
                this->current_state = DynInterfaceState::INVALID;
                break;
            }

            // enable torque on all servos
            group_sync_torque_toggle = dynamixel::GroupSyncWrite(port_handler, adapter, ADDR_MX_TORQUE_EN, TORQ_EN_PKT_LEN);
            uint8_t torque_enable = 1;
            for (int id : dynamixel_ids)
            {
                group_sync_torque_toggle.addParam(id, &torque_enable);
            }
            
            if (check_dxl_result(0, group_sync_torque_toggle.txPacket()) != DynamixelErrorCodes::SUCCESS)
            {
                this->current_state = DynInterfaceState::INVALID;
                break;
            }
            group_sync_torque_toggle.clearParam();

            // Setup bulk reader (since it doesnt' change)
            group_position_read = dynamixel::GroupBulkRead(port_handler, adapter);
            for (int id : dynamixel_ids)
            {
                group_position_read.addParam(id, ADDR_MX_POS_GET, POS_GET_PKT_LEN);
            }

            // initialize group velocity writer
            group_sync_vel_set = dynamixel::GroupSyncWrite(port_handler, adapter, ADDR_MX_VEL_SET, VEL_SET_PKT_LEN);
            
            this->current_state = DynInterfaceState::IDLE;

            break;
        }

        case DynInterfaceState::IDLE:
        {
            this->cmd_finished = false;

            if (this->run_command)
            {
                this->current_state = DynInterfaceState::SEND_INSTR;
            }

            break;
        }

        case DynInterfaceState::SEND_INSTR:
        {
            this->run_command = false;

            if (run_velocity_command() != DynamixelErrorCodes::SUCCESS)
            {
                this->current_state = DynInterfaceState::INVALID;
                break;
            }
            else
            {
                this->current_state = DynInterfaceState::READ_DATA;
            }
            
            break;
        }

        case DynInterfaceState::READ_DATA:
        {
            if (read_pos_data() != DynamixelErrorCodes::SUCCESS)
            {
                this->current_state = DynInterfaceState::INVALID;
                break;
            }

            this->current_state = DynInterfaceState::STOP_MOTORS;

            break;
        }

        case DynInterfaceState::STOP_MOTORS:
        {
            if (compare_pos_data() != DynamixelErrorCodes::SUCCESS)
            {
                this->current_state = DynInterfaceState::INVALID;
                break;
            }

            bool all_dyns_finished = true;
            for (uint8_t i = 0; i < NUM_DYNAMIXELS; i++)
            {
                if (!finished[i])
                {
                    all_dyns_finished = false;
                }
            }
            if (all_dyns_finished)
            {
                this->current_state = DynInterfaceState::FINISH;
            }
            else
            {
                this->current_state = DynInterfaceState::READ_DATA;
            }
            
            break;
        }

        case DynInterfaceState::FINISH:
        {
            this->cmd_finished = true;
            this->current_state = DynInterfaceState::IDLE;

            break;
        }

        case DynInterfaceState::SHUTDOWN:
        {
            uint8_t torque_disable = 0;
            for (int id : dynamixel_ids)
            {
                group_sync_torque_toggle.addParam(id, &torque_disable);
            }
            group_sync_torque_toggle.txPacket();

            this->port_handler->closePort();

            break;
        }
        
        default:
        {
            std::cerr << "Dynamixel Interface in invalid state! Errcode = " << errcode << std::endl;
            break;
        }
    }
}

bool is_inside_array(const uint8_t *arr, uint8_t e, uint8_t num_elems)
{
    for (int i = 0; i < num_elems; i++)
    {
        uint8_t c = arr[i];
        if (e == c)
        {
            return true;
        }
    }
    return false;
}

DynamixelErrorCodes DynamixelInterface::run_velocity_command()
{
    // run bulk write on the velocity commands from the instruction
    uint16_t *velocity_commands = instr.goal_velocities;
    for (int i = 0; i < NUM_DYNAMIXELS; i++)
    {
        uint16_t vel = velocity_commands[i];
        if(is_inside_array(reversal_ids, dynamixel_ids[i], num_reversal))
        {
            vel += 1024;
        }

        uint8_t vel_data[VEL_SET_PKT_LEN] = { DXL_LOBYTE(vel), DXL_HIBYTE(vel) };

        group_sync_vel_set.addParam(dynamixel_ids[i], vel_data);
    }

    uint16_t dxl_comm_res = group_sync_vel_set.txPacket();
    group_sync_vel_set.clearParam();

    if (check_dxl_result(0, dxl_comm_res) != DynamixelErrorCodes::SUCCESS)
    {
        return DynamixelErrorCodes::VEL_CMD_ERR;
    }

    return DynamixelErrorCodes::SUCCESS;
}

DynamixelErrorCodes DynamixelInterface::read_pos_data()
{
    // run bulk read on current dynamixel positions, offset them by the offset array
    uint16_t dxl_comm_res = group_position_read.txRxPacket();
    if (check_dxl_result(0, dxl_comm_res) != DynamixelErrorCodes::SUCCESS)
    {
        return DynamixelErrorCodes::POS_READ_ERR;
    }
    
    for (int i = 0; i < NUM_DYNAMIXELS; i++)
    {
        uint8_t id = dynamixel_ids[i];
        uint16_t current_pos = group_position_read.getData(id, ADDR_MX_POS_GET, POS_GET_PKT_LEN);
        uint16_t adjusted_pos = (current_pos + dynamixel_offsets[i]) % DYN_ROTATION_TICKS;

        pos_data[i] = adjusted_pos;
    }

    return DynamixelErrorCodes::SUCCESS;
}

DynamixelErrorCodes DynamixelInterface::compare_pos_data()
{
    // iterate through pos_data and see if any motors need to be stopped
    uint16_t *desired_positions = instr.goal_positions;
    for (int i = 0; i < NUM_DYNAMIXELS; i++)
    {
        if (finished[i]) { continue; }

        uint8_t id = dynamixel_ids[i];
        uint16_t current_pos = pos_data[i];
        uint16_t desired_pos = desired_positions[i];

        if (abs(current_pos - desired_pos) < goal_tol)
        {
            // stop the motor
            finished[i] = true;
            uint8_t dxl_err;
            int16_t dxl_comm_res = adapter->write2ByteTxRx(port_handler, id, ADDR_MX_VEL_SET, 0, &dxl_err);
            if (check_dxl_result(dxl_err, dxl_comm_res) != DynamixelErrorCodes::SUCCESS)
            {
                return DynamixelErrorCodes::VEL_CMD_ERR;
            }
        }
    }

    return DynamixelErrorCodes::SUCCESS;
}

DynamixelErrorCodes DynamixelInterface::check_dxl_result(uint8_t dxl_err, int16_t dxl_comm_res)
{
    if (dxl_err)
    {
        std::cerr << "DXL Error, " << std::to_string(dxl_err) << std::endl;
        errcode = dxl_err;
        return DynamixelErrorCodes::DXL_ERR;
    }
    if (dxl_comm_res != COMM_SUCCESS)
    {
        std::cerr << "DXL Comm Error, " << adapter->getTxRxResult(dxl_comm_res) << std::endl;
        errcode = dxl_comm_res;
        return DynamixelErrorCodes::DXL_COMM_ERR;
    }

    return DynamixelErrorCodes::SUCCESS;
}