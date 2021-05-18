/*
 * bootcode.c
 *
 *  Created on: May 10, 2021
 *      Author: ajg1079
 */
#include "main.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"

#include "font.h"
#include "bootcode.h"

#include <string.h>

/* Start @ of user Flash area */
#define FLASH_USER_START_ADDR					ADDR_FLASH_SECTOR_6	
/* End @ of user Flash area : sector start address + sector size -1 */
#define FLASH_USER_END_ADDR						ADDR_FLASH_SECTOR_23  +  GetSectorSize(ADDR_FLASH_SECTOR_23) -1

//flash write Test Data
#define DATA_32									((uint32_t)0x00000000)

// UART Packet limit size
#define	PACKETLIMITELENGTH						256

// Wait update signal -> 5000ms (5초)
#define APPLICATION_UPDATE_WAITING_TIME_VALU	5000

// jump address 
#define APPLICATION_ADDRESS						FLASH_USER_START_ADDR

//Graphic buffer
uint8_t gpubuffer[512] = "";

// UART Rx DMA Buffer
uint8_t sUART_DMA_ReceiveBuffer[PACKETLIMITELENGTH];

// uart 수신 받고 나서 임시 저장하는 변수 -> 정리 대상
uint8_t sTempStringData[PACKETLIMITELENGTH] = "";

// Data 패킷 index 저장용 변수
char sPacketIndex[32] = "";
// 실제 업데이트 내용 담을 변수
char sPacketData[PACKETLIMITELENGTH] = "";

// info 패킷으로 받은 패킷 크기 정보 배열 형식
char sPacketSize[32] = "";
// info 패킷으로 받은 패킷 개수 정보 배열 형식
char sPacketCount[32] = "";

// i2C OLED에 표현할 데이터 임시 변수
char sDisplayLogData[32] ="";

// command
char command[32] = "";

// uart 수신 버퍼 index
int iUartRxCallbackIndex = 0;

// bootloader waiting timer
unsigned int iGlobalTimer;

// bootloader waiting timer start flag
unsigned int iGlobalTimerStart;

/* Convert int and String 4 Bytes */
union uIntegerConvert
{
	int uIntegerData;
	char uStringData[4];
};

/*
 *  @brief	Bootloader Main Code
 *  @param	None
 *  @retval	function status
 */
int bootcode(void)
{
	//HAL function result
	HAL_StatusTypeDef res = HAL_OK;

	//for문 변수
	int i = 0,x= 0,y= 0,z= 0;

	//명령어 마지막 부분 index
	int iCommandEndIndex = 0;
	// 패킷의 마지막 부분
	int iPaketEndIndex = 0;
	// 패킷 내부 ','개수
	int iCommaIndex_index = 0;
	// ','위치
	int iCommaIndex[4] = {0x00,};

	// 수신된 패킷에 있던 checksum 값
	char crc_xor = 0;
	// 수신된 버퍼를 기준으로 계산한 checksum 값
	char crc_xor_calres = 0;

	//패킷 데이터 부분 크기
	int iPacketSize = 0;
	// 데이터 패킷 보낼 수량
	int iPacketCount = 0;

	//수신된 패킷의 순번
	int iPacketIndex = 0;

	//write 할 Flash 시작주소와 마지막 주소 
	int iFlashMemoryAddress = FLASH_USER_START_ADDR;
	int iFlashMemoryBackupAddress = FLASH_USER_START_ADDR;

	// update 시작 여부. 현재 부트로더는 5초동안 업데이트 신호 안받으면 
	//app으로 넘어가기 때문에 자동으로 넘어가기 전 업데이트 신호 받으면 1로 set
	int iUpdateStartFlag = 0;

	//info 패킷 처리여부
	int iInfopacketFlag = 0;

	//UART 수신 버퍼 index 0으로 초기화
	iUartRxCallbackIndex = 0;
	// UART 수신 버퍼 초기화
	memset(sUART_DMA_ReceiveBuffer,0x00,sizeof(sUART_DMA_ReceiveBuffer));

	//i2C OLED Display init
	init_display();

	//i2C OLED 버퍼 초기화
	memset(gpubuffer,0x00,sizeof(gpubuffer));
	//i2C OLED 버퍼 (0,0)위치에 "Boot Loader" 표시
	fDisplayString(0,0,gpubuffer,"Boot Loader");
	//i2C OLED 버퍼 적용
	ssd1306_drawingbuffer(gpubuffer);

	// UART2 수신 interrupt 사용
	__HAL_UART_ENABLE_IT(&huart2,UART_IT_RXNE);

	// 업데이트 여부 카운트 시작
	iGlobalTimerStart = 1;

	while (1)
	{
		//info 패킷에 대해 오류없이 처리하면
		if (iInfopacketFlag == 1)
		{
			printf("[MCU]INFO,ACK\r\n");
			z++;
			iInfopacketFlag = 0;
		}

		//update 완료되면
		if (z == -1)
		{
			printf("[MCU]END,ACK\r\n");
			NVIC_SystemReset();
		}

		// waiting이 5초가 안되고 업데이트 시작 플래그가 1이면 처리
		if ((iGlobalTimer <= APPLICATION_UPDATE_WAITING_TIME_VALU) || (iUpdateStartFlag == 1))
		{
			// 서버에 업로드 준비 완료 메세지 0.5초 간격으로 전송. 전송 간격 도중에 업데이트 시작되면 자동 차단
			if (z == 0)
			{
				printf("[MCU]READY,ACK\r\n");
				HAL_Delay(500);
			}

			//패킷 검사 상위바이트 -> 하위바이트 검색
			for (x = PACKETLIMITELENGTH; x > 0; x--)
			{
				// "\r\n"이 연속으로 최초 검출 되면 메세지 날라온걸로 판별
				if ((sUART_DMA_ReceiveBuffer[x - 1] == '\r') && (sUART_DMA_ReceiveBuffer[x] == '\n'))
				{
					//명령어 마지막 부분 index 초기화
					iCommandEndIndex = 0;
					// 패킷의 마지막 부분은 현재 수신 버퍼의 \n 위치
					iPaketEndIndex = x;

					// 명령어 버퍼 초기화
					memset(command, 0x00, sizeof(command));

					// 명령어 분류 하기 위해 명령어 마지막 부분 찾기
					for (i = 0; i < x; i++)
					{
						if (sUART_DMA_ReceiveBuffer[i] == ']')
						{
							//명령어 마지막 부분 index 잡고
							iCommandEndIndex = i;
							for (y = 0; y < iCommandEndIndex - 1; y++)
							{
								command[y] = sUART_DMA_ReceiveBuffer[y + 1];
							}
							i = x + 1;
						}
					}

					// 명령어 구별
					if (fCompareFunction(command, "INFO", 4) == 0)
					{
						// update 시작
						iUpdateStartFlag = 1;

						// Application 영역 포맷
						app_Partition_erase();

						//command 부분을 제외한 부분을 임시 버퍼로 복사
						memset(sTempStringData, 0x00, sizeof(sTempStringData));
						memcpy(sTempStringData, sUART_DMA_ReceiveBuffer + (iCommandEndIndex + 1), iPaketEndIndex - (iCommandEndIndex + 1));

						// ','부분 개수 파악 변수 초기화
						iCommaIndex_index = 0;

						// ','위치 파악, info에서는 ','가 2개 오는데 가운데 데이터에 숫자형태 외 들어올 
						//데이터가 없기 때문에 순차적으로 찾는다
						for (y = 0; y < iPaketEndIndex - (iCommandEndIndex + 1); y++)
						{
							if (sTempStringData[y] == ',')
							{
								iCommaIndex[iCommaIndex_index] = y;
								iCommaIndex_index++;
							}
						}
						// ','가 2개 검출된게 맞다면
						if (iCommaIndex_index == 2)
						{
							// 패킷 크기와 개수 받을 배열 변수
							memset(sPacketSize, 0x00, sizeof(sPacketSize));
							memset(sPacketCount, 0x00, sizeof(sPacketCount));

							// 패킷 크기
							memcpy(sPacketSize, sTempStringData, iCommaIndex[0]);

							// 패킷 개수
							memcpy(sPacketCount, sTempStringData + (iCommaIndex[0] + 1), (iCommaIndex[1] - (iCommaIndex[0] + 1)));

							// 패킷에 있던 Checksum
							crc_xor = sTempStringData[iCommaIndex[1] + 1];

							//패킷 크기 와 개수 배열변수 데이터를 정수형으로 변환
							iPacketSize = fConvertStringToInt32(sPacketSize);
							iPacketCount = fConvertStringToInt32(sPacketCount);

							// 패킷 데이터 Checksum
							memcpy(sTempStringData, sTempStringData, (iCommaIndex[1]));
							crc_xor_calres = crc_xor_calculation(sTempStringData, (iCommaIndex[1]));

							//패킷 crc와 계산한 crc 비교
							if (crc_xor_calres == crc_xor)
							{
								iInfopacketFlag = 1;
							}
							else
							{
								iInfopacketFlag = 0;
							}

							//UART 수신 버퍼 초기화
							memset(sUART_DMA_ReceiveBuffer, 0x00, sizeof(sUART_DMA_ReceiveBuffer));
							iUartRxCallbackIndex = 0;

							// UART 수신 인터럽트 시작
							__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);
						}
					}
					else if (fCompareFunction(command, "DATA", 4) == 0)
					{
						//현재 기록할 Flash Address 백업
						iFlashMemoryBackupAddress = iFlashMemoryAddress;

						// Packet size와 Length 구하는 부분
						iCommaIndex_index = 0;

						// 첫번째 ','검출, 이때는 배열 초반부에 있기때문에 하위배열부터 상위
						//배열까지 순차적 검색
						for (y = 0; y < iPaketEndIndex; y++)
						{
							if (sUART_DMA_ReceiveBuffer[y] == 0x2c)
							{
								iCommaIndex[0] = y;
								y = PACKETLIMITELENGTH;
								iCommaIndex_index++;
							}
						}

						//두번째 ','검출, 데이터 패킷에도 ','가 존재 할 가능성이 있기때문에
						//상위 바이트부터 검색, 단 CRC도 0x2c로 나올 가능성이 있기 때문에
						// \r,\n,CRC 구역을 제외하고 검색한다
						for (y = iPaketEndIndex - 3; y > 0; y--)
						{
							if (sUART_DMA_ReceiveBuffer[y] == 0x2c)
							{
								iCommaIndex[1] = y;
								y = -1;
								iCommaIndex_index++;
							}
						}

						// 데이터 패킷도 ','가 2개 나와야 처리 한다
						if (iCommaIndex_index == 2)
						{
							// 패킷에 있는 현재 데이터 패킷 번호 문자열
							memcpy(sPacketIndex, sUART_DMA_ReceiveBuffer + iCommandEndIndex + 1, iCommaIndex[0]);

							// 패킷 데이터
							memcpy(sPacketData, sUART_DMA_ReceiveBuffer + (iCommaIndex[0] + 1 + iCommandEndIndex), (iCommaIndex[1] - (iCommaIndex[0] + 1)));

							// 패킷에 있는 Checksum
							crc_xor = sUART_DMA_ReceiveBuffer[iCommaIndex[1] + 1];

							// 현재 데이터 패킷 번호 문자열을 정수로 변환
							iPacketIndex = fConvertStringToInt32(sPacketIndex);

							//패킷 크기만큼 Falsh Address 증가
							for (z = 0; z < iPacketSize; z++)
							{
								iFlashMemoryAddress++;
							}

							// 패킷에 있던 데이터 Flash에 저장
							res = flash_wrtie(iFlashMemoryBackupAddress, iFlashMemoryAddress, sUART_DMA_ReceiveBuffer + (iCommaIndex[0] + 1));
							if (res != 0)
							{
								//Flash 쓰기 에러
								printf("flash_wrtie Err\r\n");
							}

							// 전체 데이터 기반으로 Checksum 계산
							crc_xor_calres = crc_xor_calculation(sUART_DMA_ReceiveBuffer + (iCommandEndIndex + 1), (iCommaIndex[1] - (iCommandEndIndex + 1)));

							//UART 수신 버퍼 초기화
							memset(sUART_DMA_ReceiveBuffer, 0x00, sizeof(sUART_DMA_ReceiveBuffer));
							iUartRxCallbackIndex = 0;

							// UART 수신 인터럽트 활성
							__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

							// Chaecksum이 같다면
							if (crc_xor_calres == crc_xor)
							{
								printf("[MCU]DATA,ACK\r\n");

								// 마지막 패킷이면
								if (iPacketIndex >= iPacketCount)
								{
									z = -1;
								}
							}
							//checksum이 다르면
							else
							{
								//시작 주소로 롤백
								iFlashMemoryAddress = iFlashMemoryBackupAddress;
								printf("[MCU]DATA,NACK\r\n");
							}
						}
					}
				}
			}
			HAL_Delay(75);
		}
		else
		{
			// jump to application
			jump_to_application(FLASH_USER_START_ADDR);
		}
	}
	return 0;
}

/*
 *  @brief	Application 영역 섹터 지우는 함수
 *  @param	None
 *  @retval	쓰기 성공 여부, 0은 성공, 나머지는 실패
 */
int app_Partition_erase(void)
{
	HAL_StatusTypeDef res = HAL_OK;

	uint32_t FirstSector = 0;
	uint32_t NbOfSectors = 0;
	uint32_t SECTORError = 0;

	static FLASH_EraseInitTypeDef EraseInitStruct;

	//flash unlock
	HAL_FLASH_Unlock();

	/* Get the 1st sector to erase */
	FirstSector = GetSector(FLASH_USER_START_ADDR);

	/* Get the number of sector to erase from 1st sector*/
	NbOfSectors = GetSector(FLASH_USER_END_ADDR) - FirstSector + 1;

	EraseInitStruct.TypeErase     = FLASH_TYPEERASE_SECTORS;
	EraseInitStruct.VoltageRange  = FLASH_VOLTAGE_RANGE_3;
	EraseInitStruct.Sector        = FirstSector;
	EraseInitStruct.NbSectors     = NbOfSectors;

	//flash Erase
	res = HAL_FLASHEx_Erase(&EraseInitStruct, &SECTORError);
	if(res != HAL_OK)
	{
		printf("[MCU][Error]HAL_FLASHEx_Erase=%d\r\n",res);
		return res;
	}

	//flash unlock
	HAL_FLASH_Lock();

	return res;
}

/*
 *  @brief	Flash 쓰기 테스트
 *  @param	None
 *  @retval None
 */
int write_test(void)
{
	HAL_StatusTypeDef res = HAL_OK;

	uint32_t Address = FLASH_USER_START_ADDR;

	//flash unlock
	HAL_FLASH_Unlock();

	while(Address < FLASH_USER_END_ADDR)
	{
		res = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, Address, DATA_32);
		if(res != HAL_OK)
		{
			printf("[MCU][Error]HAL_FLASHEx_Erase=%d\r\n",res);
		}
		Address = Address + 4;
	}

	//flash unlock
	HAL_FLASH_Lock();

	return res;
}

/*
 *  @brief	Flash 특정 주소에 데이터 쓰기 함수
 *  @param	Address		write flash start address
 *			end_address	write flash end address
 *			data		쓸 데이터 배열 시작 주소
 *  @retval	쓰기 성공 여부, 0은 성공, 나머지는 실패
 */
int flash_wrtie(uint32_t Address, uint32_t end_address, char *data)
{
	HAL_StatusTypeDef res = HAL_OK;
	int index = 0;
	int buffer = 0;

	//flash unlock
	HAL_FLASH_Unlock();

	while(Address < end_address)
	{
		buffer=fConvertString4BytesToInteger32Type(data + index);

		res = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, Address, buffer);
		//res = HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, Address, data[index]);
		if(res != HAL_OK)
		{
			printf("[MCU][ERROR]%d\r\n",res);
			return res;
		}
		Address = Address + 4;
		index = index + 4;
	}

	//flash unlock
	HAL_FLASH_Lock();

	return res;
}

/*
 *  @brief	Gets sector Size
 *  @param	Sector	select sector
 *  @retval	The size of a given sector
 */
static uint32_t GetSectorSize(uint32_t Sector)
{
	uint32_t sectorsize = 0x00;
	if((Sector == FLASH_SECTOR_0) || (Sector == FLASH_SECTOR_1) || (Sector == FLASH_SECTOR_2) ||\
		 (Sector == FLASH_SECTOR_3) || (Sector == FLASH_SECTOR_12) || (Sector == FLASH_SECTOR_13) ||\
		 (Sector == FLASH_SECTOR_14) || (Sector == FLASH_SECTOR_15))
	{
		sectorsize = 16 * 1024;
	}
	else if((Sector == FLASH_SECTOR_4) || (Sector == FLASH_SECTOR_16))
	{
		sectorsize = 64 * 1024;
	}
	else
	{
		sectorsize = 128 * 1024;
	}
	return sectorsize;
}

/*
 *	@brief	Gets the sector of a given address
 *	@param	Address	select flash sector
 *	@retval	The sector of a given address
 */
static uint32_t GetSector(uint32_t Address)
{
	uint32_t sector = 0;

	if((Address < ADDR_FLASH_SECTOR_1) && (Address >= ADDR_FLASH_SECTOR_0))
	{
		sector = FLASH_SECTOR_0;
	}
	else if((Address < ADDR_FLASH_SECTOR_2) && (Address >= ADDR_FLASH_SECTOR_1))
	{
		sector = FLASH_SECTOR_1;
	}
	else if((Address < ADDR_FLASH_SECTOR_3) && (Address >= ADDR_FLASH_SECTOR_2))
	{
		sector = FLASH_SECTOR_2;
	}
	else if((Address < ADDR_FLASH_SECTOR_4) && (Address >= ADDR_FLASH_SECTOR_3))
	{
		sector = FLASH_SECTOR_3;
	}
	else if((Address < ADDR_FLASH_SECTOR_5) && (Address >= ADDR_FLASH_SECTOR_4))
	{
		sector = FLASH_SECTOR_4;
	}
	else if((Address < ADDR_FLASH_SECTOR_6) && (Address >= ADDR_FLASH_SECTOR_5))
	{
		sector = FLASH_SECTOR_5;
	}
	else if((Address < ADDR_FLASH_SECTOR_7) && (Address >= ADDR_FLASH_SECTOR_6))
	{
		sector = FLASH_SECTOR_6;
	}
	else if((Address < ADDR_FLASH_SECTOR_8) && (Address >= ADDR_FLASH_SECTOR_7))
	{
		sector = FLASH_SECTOR_7;
	}
	else if((Address < ADDR_FLASH_SECTOR_9) && (Address >= ADDR_FLASH_SECTOR_8))
	{
		sector = FLASH_SECTOR_8;
	}
	else if((Address < ADDR_FLASH_SECTOR_10) && (Address >= ADDR_FLASH_SECTOR_9))
	{
		sector = FLASH_SECTOR_9;
	}
	else if((Address < ADDR_FLASH_SECTOR_11) && (Address >= ADDR_FLASH_SECTOR_10))
	{
		sector = FLASH_SECTOR_10;
	}
	else if((Address < ADDR_FLASH_SECTOR_12) && (Address >= ADDR_FLASH_SECTOR_11))
	{
		sector = FLASH_SECTOR_11;
	}
	else if((Address < ADDR_FLASH_SECTOR_13) && (Address >= ADDR_FLASH_SECTOR_12))
	{
		sector = FLASH_SECTOR_12;
	}
	else if((Address < ADDR_FLASH_SECTOR_14) && (Address >= ADDR_FLASH_SECTOR_13))
	{
		sector = FLASH_SECTOR_13;
	}
	else if((Address < ADDR_FLASH_SECTOR_15) && (Address >= ADDR_FLASH_SECTOR_14))
	{
		sector = FLASH_SECTOR_14;
	}
	else if((Address < ADDR_FLASH_SECTOR_16) && (Address >= ADDR_FLASH_SECTOR_15))
	{
		sector = FLASH_SECTOR_15;
	}
	else if((Address < ADDR_FLASH_SECTOR_17) && (Address >= ADDR_FLASH_SECTOR_16))
	{
		sector = FLASH_SECTOR_16;
	}
	else if((Address < ADDR_FLASH_SECTOR_18) && (Address >= ADDR_FLASH_SECTOR_17))
	{
		sector = FLASH_SECTOR_17;
	}
	else if((Address < ADDR_FLASH_SECTOR_19) && (Address >= ADDR_FLASH_SECTOR_18))
	{
		sector = FLASH_SECTOR_18;
	}
	else if((Address < ADDR_FLASH_SECTOR_20) && (Address >= ADDR_FLASH_SECTOR_19))
	{
		sector = FLASH_SECTOR_19;
	}
	else if((Address < ADDR_FLASH_SECTOR_21) && (Address >= ADDR_FLASH_SECTOR_20))
	{
		sector = FLASH_SECTOR_20;
	}
	else if((Address < ADDR_FLASH_SECTOR_22) && (Address >= ADDR_FLASH_SECTOR_21))
	{
		sector = FLASH_SECTOR_21;
	}
	else if((Address < ADDR_FLASH_SECTOR_23) && (Address >= ADDR_FLASH_SECTOR_22))
	{
		sector = FLASH_SECTOR_22;
	}
	else /* (Address < FLASH_END_ADDR) && (Address >= ADDR_FLASH_SECTOR_23) */
	{
		sector = FLASH_SECTOR_23;
	}
	return sector;
}

/*
 *  @brief	i2C OLED Command 전송
 *  @param	c	1Byte Command
 *  @retval	None
 */
void ssd1306_W_Command(uint8_t c)
{
    HAL_StatusTypeDef res;

    uint8_t buffer[2]={0};		//Control Byte + Command Byte
    buffer[0]=(0<<7)|(0<<6);	//Co=0 , D/C=0
    buffer[1]=c;

    res = HAL_I2C_Master_Transmit(&hi2c2,(uint16_t)(0x3C)<<1,(uint8_t*)buffer,2,9999);
    if(res != HAL_OK)
    {
    	printf("Err:%d\r\n",res);
    }
	while (HAL_I2C_GetState(&hi2c2) != HAL_I2C_STATE_READY);
}

/*
 *  @brief	i2C OLED Command 전송
 *  @param	data_buffer	전송 할 데이터 배열 시작 주소
 *			buffer_size	전송 할 Data 크기
 *  @retval	None
 */
void ssd1306_W_Data(uint8_t* data_buffer, uint16_t buffer_size)
{
	HAL_StatusTypeDef res;
	res = HAL_I2C_Mem_Write(&hi2c2,(uint16_t)(0x3C<<1),0x40,1,data_buffer,buffer_size,9999);
	if(res != HAL_OK)
	{
		printf("i2C Err:%d\r\n",res);
	}
	while (HAL_I2C_GetState(&hi2c2) != HAL_I2C_STATE_READY);
}

/*
 *  @brief	i2C OLED 초기화 함수
 *  @param	None
 *  @retval	None
 */
void init_display(void)
{
    // Init sequence for 128x64 OLED module
	ssd1306_W_Command(0xAE);		// 0xAE
    ssd1306_W_Command(0xD5);		// 0xD5
    ssd1306_W_Command(0x80);		// the suggested ratio 0x80
    ssd1306_W_Command(0xA8);		// 0xA8
    ssd1306_W_Command(0x3F);
    ssd1306_W_Command(0xD3);		// 0xD3
    ssd1306_W_Command(0x00);		// no offset
    ssd1306_W_Command(0x40);		// line #0
    ssd1306_W_Command(0x8D); 		// 0x8D
    ssd1306_W_Command(0x14); 		// using internal VCC
    ssd1306_W_Command(0x20);		// 0x20
    ssd1306_W_Command(0x00);		// 0x00 horizontal addressing
    ssd1306_W_Command(0xA0 | 0x1); 	// rotate screen 180
    ssd1306_W_Command(0xC8); 		// rotate screen 180
    ssd1306_W_Command(0xDA);    	// 0xDA
    ssd1306_W_Command(0x02);
    ssd1306_W_Command(0x81);		// 0x81
    ssd1306_W_Command(0xCF);
    ssd1306_W_Command(0xd9); 		// 0xd9
    ssd1306_W_Command(0xF1);
    ssd1306_W_Command(0xDB);		// 0xDB
    ssd1306_W_Command(0x40);
    ssd1306_W_Command(0xA4);		// 0xA4
    ssd1306_W_Command(0xA6); 		// 0xA6
    ssd1306_W_Command(0xAF);		//switch on OLED
}

/*
 *  @brief	i2C OLED에 그래픽 버퍼 전달 함수
 *  @param	sdata	그래픽 버퍼 시작 주소
 *  @retval	None
 */
void ssd1306_drawingbuffer(char *sdata)
{
	ssd1306_W_Command(0x00);
	ssd1306_W_Command(0x10);

	for(uint8_t i=4;i<8;i++)
	{
		ssd1306_W_Command(0xB0+i);
		ssd1306_W_Data(sdata + (128 * (i-4)),128);
	}
}

/*
 *  @brief	display 버퍼에 문자 삽입 역할 함수
 *  @param	iLocationX		표기를 시작할 X 좌표
 *			iLocationY		표기를 시작할 Y 좌표
 *			cData			표기 할 문자
 *			displaybuffer	디스플레이 버퍼 배열 시작 주소
 *  @retval	None
 */
void fDisplayChar(int iLocationX, int iLocationY, char cData, char* displaybuffer)
{
	/*
	 *	사용하는 폰트는 세로가 바이트로 구별 (개별 앨리먼트)되어 있는데 가로는 비트단위로
	 *	제작되어 있다. 문제는 사용하는 OLED 드라이버칩 특성상 가로가 바이트 단위고 세로가
	 *	비트 단위라 변환해줘야 한다
	 */

	 // 입력하는 문자를 사용하는 폰트 데이터 index에 맞게 변환
	int iIndexChar = (cData - 32) * 12;
	//For문 사용 변수
	int x, y, z;
	// 실제 좌표와 드라이버칩의 메모리 위치 환산 위한 변수
	int iLocationSumY = 0;
	char buf = 0b00000001;
	char nbuf = 0b00000001;

	//특이하게 byte by byte로 픽셀이 매칭이 아닌 행은 Bit로, 열은 Byte로 구분 되어 있다.
	//참고 자료 : http://www.datasheet.kr/ic/1017173/SSD1309-datasheet-pdf.html

	// 폰트 세로크기
	for (y = 0; y < 12; y++)
	{
		// 폰트 가로크기
		for (x = 0; x < 1; x++)
		{
			//폰트 가로는 비트로 구성
			for (z = 7; z >= 0; z--)
			{
				//폰트 가로축을 1비트씩 읽어서 On해야 되는 픽셀이 있다면
				if (((Font12_Table[iIndexChar + (y + x)] >> z) & 0x1) == 1)
				{
					// 문자의 시작하려는 y좌표와 현재 읽어들인 문자의 y좌표 합 계산
					//0~31까지의 실제 OLED 좌표와 드라이버칩 메모리 위치 환산 하기 위해 계산해야 한다
					iLocationSumY = y + iLocationY;

					//즉 iLocationSumY가 0~7,8~15,16~23,24~31 각각 메모리상으로는 1개의 char 형태로
					//묶여있는 형태다
					if (iLocationSumY < 8)
					{
						//각 영역 내부에 표시하기 위한 shift
						buf = 0b00000001 << iLocationSumY;
						//버퍼 메모리에 해당 위치 변환된 부분에 1을 기록
						displaybuffer[(iLocationY * 128) + (iLocationX + (7 - z))] = displaybuffer[(iLocationY * 128) + (iLocationX + (7 - z))] | buf;
					}
					else if ((iLocationSumY >= 8) && (iLocationSumY < 16))
					{
						iLocationSumY = iLocationSumY - 8;
						buf = 0b00000001 << iLocationSumY;
						displaybuffer[((iLocationY + 1) * 128) + (iLocationX + (7 - z))] = displaybuffer[((iLocationY + 1) * 128) + (iLocationX + (7 - z))] | buf;
					}
					else if ((iLocationSumY >= 16) && (iLocationSumY < 24))
					{
						iLocationSumY = iLocationSumY - 16;
						buf = 0b00000001 << iLocationSumY;
						displaybuffer[((iLocationY + 2) * 128) + (iLocationX + (7 - z))] = displaybuffer[((iLocationY + 2) * 128) + (iLocationX + (7 - z))] | buf;
					}
					else if ((iLocationSumY >= 24) && (iLocationSumY < 32))
					{
						iLocationSumY = iLocationSumY - 24;
						buf = 0b00000001 << iLocationSumY;
						displaybuffer[((iLocationY + 3) * 128) + (iLocationX + (7 - z))] = displaybuffer[((iLocationY + 3) * 128) + (iLocationX + (7 - z))] | buf;
					}
				}
				// 0으로 처리되는 픽셀이면
				else if (((Font12_Table[iIndexChar + (y + x)] >> z) & 0x1) == 0)
				{
					iLocationSumY = y + iLocationY;
					if (iLocationSumY < 8)
					{
						buf = 0b00000001 << iLocationSumY;
						// 해당 부분 픽셀은 Off이므로 Not연산으로 비트 반전
						nbuf = ~buf;
						//And연산으로 꺼버린다
						displaybuffer[(iLocationY * 128) + (iLocationX + (7 - z))] = (displaybuffer[(iLocationY * 128) + (iLocationX + (7 - z))] | buf) & nbuf;
					}
					else if ((iLocationSumY >= 8) && (iLocationSumY < 16))
					{
						iLocationSumY = iLocationSumY - 8;
						buf = 0b00000001 << iLocationSumY;
						nbuf = ~buf;
						displaybuffer[((iLocationY + 1) * 128) + (iLocationX + (7 - z))] = (displaybuffer[((iLocationY + 1) * 128) + (iLocationX + (7 - z))] | buf) & nbuf;
					}
					else if ((iLocationSumY >= 16) && (iLocationSumY < 24))
					{
						iLocationSumY = iLocationSumY - 16;
						buf = 0b00000001 << iLocationSumY;
						nbuf = ~buf;
						displaybuffer[((iLocationY + 2) * 128) + (iLocationX + (7 - z))] = (displaybuffer[((iLocationY + 2) * 128) + (iLocationX + (7 - z))] | buf) & nbuf;
					}
					else if ((iLocationSumY >= 24) && (iLocationSumY < 32))
					{
						iLocationSumY = iLocationSumY - 24;
						buf = 0b00000001 << iLocationSumY;
						nbuf = ~buf;
						displaybuffer[((iLocationY + 3) * 128) + (iLocationX + (7 - z))] = (displaybuffer[((iLocationY + 3) * 128) + (iLocationX + (7 - z))] | buf) & nbuf;
					}
				}
			}
		}
	}
}

/*
 *  @brief	display에 string 뿌리기 위한 함수
 *  @param	iLocationX		표기를 시작할 X 좌표
 *			iLocationY		표기를 시작할 Y 좌표
 *			displaybuffer	디스플레이 버퍼 배열 시작 주소
 *			p				문자열 주소
 *  @retval	None
 */
void fDisplayString(int iLocationX, int iLocationY, char *displaybuffer, const char *p, ...)
{
	// 문자열 내부 행 좌표
	int x = 0;
	// NULL문자 나올때까비 반복
	while (*p != '\0')
	{
		fDisplayChar(iLocationX + x, iLocationY, *p, displaybuffer);
		//문자"열"이므로 다음 문자는 폰트의 가로 크기인 8만큼 더해준다
		x = x + 8;
		//다음 문자 호출
		p++;
	}
}

/*
 *  @brief  문자열 비교 함수
 *  @param  source 비교할 문자열 시작 주소
 *          target 비교대상 문자열 시작 주소
 *          iSize  비교할 길이
 *  @retval 비교 대상 결과 값, 같으면 0 다르면 -1
 */
int fCompareFunction(char *source, char *target, int iSize)
{
	for(int i = 0 ; i < iSize ; i++ )
	{
		if(source[i] != target[i])
		{
			return -1;
		}
	}
	return 0;
}

/*
 *  @brief  문자열 형태의 숫자를 int32값으로 변환
 *  @param  source  문자열 에서 정수형으로 바꿀 문자열 시작 주소
 *  @retval int32로 변환 된 값
 */
int fConvertStringToInt32(char *source)
{
	int buf = source[0] - 0x30;
	int res = buf;
	int i = 1;
	while(1)
	{
		if((source[i] >= 0x30)&&(source[i] <= 0x39))
		{
			res = res * 10;
			buf = source[i] - 0x30;
			res = res + buf;
		}
		else
		{
			break;
		}
		i++;
	}
	return res;
}

/*
 *  @brief  CheckSum8 Xor 연산 함수
 *  @param  sData       연산 대상 문자열 시작 주소
 *          data_size   연산 대상 문자열 크기
 *  @retval CheckSum8 결과 값
 */
char crc_xor_calculation(char *sData, int data_size)
{
	char res = sData[0];
	for(int i = 1 ; i < data_size ; i++ )
	{
		res = res ^ sData[i];
	}
	return res;
}

/*
 *  @brief	int32 형태의 데이터를 char 배열로 변경, 단순히 숫자 형태가 아닌 바이트로 쪼갠 형태
 *  @param	fData	변환 대상
 *			sData	변환 하고 나서 저장 할 배열 변수 주소
 *  @retval	None
 */
void fConvertInteger32TypeToString4Bytes(int fData, unsigned char* sData)
{
	union uIntegerConvert uF;
	uF.uIntegerData = fData;
	memcpy(sData, uF.uStringData, sizeof(char) * 4);
}

/*
 *  @brief  char 배열 형식으로 날라오는 데이터를 int32 형태로 전환, union 사용.
 *  @param  sData	int32로 변환 할 배열 데이터
 *  @retval 변환 완료된 int32
 */
int fConvertString4BytesToInteger32Type(unsigned char* sData)
{
	union uIntegerConvert uint;
	memcpy(uint.uStringData, sData, sizeof(int));
	return uint.uIntegerData;
}

/*
 *  @brief	Application으로 jump하기 위한 함수
 *  @param	application_start_address	jump할 주소
 *  @retval None
 */
void jump_to_application(uint32_t application_start_address)
{
	typedef void (*fptr)(void);
	fptr jump_to_app;
	uint32_t jump_addr;
	jump_addr = *(__IO uint32_t*) (APPLICATION_ADDRESS + 4);
	jump_to_app = (fptr)jump_addr;
	__set_MSP(*(__IO uint32_t*) APPLICATION_ADDRESS);
	jump_to_app();
}

/*
 *  @brief	UART 수신 인터럽트로부터 데이터 받아와 호출 할때마다 index 증가하여 배열에 저장.
 *  @param	data	intterupt쪽에서 데이터 받아오기 위한 파라미터
 *  @retval	None
 */
void getRxBuffer(uint8_t data)
{
	sUART_DMA_ReceiveBuffer[iUartRxCallbackIndex] = data;
	iUartRxCallbackIndex++;
}

