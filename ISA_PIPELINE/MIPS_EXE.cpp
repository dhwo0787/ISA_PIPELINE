#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "Ws2_32.lib")
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <WinSock2.h>
#include <time.h>

unsigned int ch[0x400000]; // BIG MEM 
unsigned int R[32]; // REG
int pc = 0; // pc value
//
int JumpLib[100][4]; // [0]: pc , [1]: opcode , [2]: bAddr [3]: JumpCategory 
int JumpIter = 0;
//
int decode_first = 1;
int execute_first = 1;
int memaccess_first = 1;
int writeback_first = 1;
//
int rs_dist = 4;
int rt_dist = 4;
// 
int n_ins = 0;
int n_mem = 0;
int n_reg = 0;
int n_jump = 0;
int n_branch = 0;
int n_not_taken = 0;
int n_taken = 0;
int branchPredict = 0;
int branchNotPredict = 0;
//
struct control_sig {
    unsigned int aluop;
    unsigned int alusrc;
    unsigned int memRead, memWrite, memToReg, regWrite, regDst;
    unsigned int branchJ;
    unsigned int rsUse, rtUse;
};
struct Fetch_Decode_latch {
    int valid;
    unsigned int inst;
    int pc_value; //  현재 인스트럭션 pc 값
};
struct Decode_Execute_latch {
    int valid;
    unsigned int opcode;
    int pc_value;
    int ReadData1;
    int ReadData2;
    int rs; // data1 reg num
    int rd;
    int rt;
    int funct;
    int MemWriteData; 
    int WriteBackNum;
    int imm;
    int shamt;
    struct control_sig cs;
};
struct Execute_MemAccess_latch {
    int valid;
    int pc_value;
    int AluResult;
    int rs;
    int rd;
    int rt;
    int funct;
    int MemWriteData;
    int WriteBackNum;
    struct control_sig cs;
};
struct MemAccess_WriteBack_latch {
    int valid;
    int pc_value;
    int Data;
    int WriteBackNum;
    struct control_sig cs;
};

unsigned int cal_opc(unsigned int a) {
    a = a & 0xfc000000;
    a = a >> 26;
    a = a & 0x0000003F;
    return a;
}
int cal_jump(unsigned int pc, int ins) {
    pc = pc + 4;
    pc = pc & 0xf0000000;
    ins = ins & 0x03ffffff;
    ins = ins << 2;
    ins = ins | pc;
    return ins;
}
int cal_Baddr(unsigned int ins) {
    ins = ins & 0x0000ffff;
    unsigned int a = ins >> 15;

    if (a == 1) {
        ins = ins | 0xffff0000;
        ins = ins << 2;
        return ins;
    }
    else {
        return ins << 2;
    }
}
unsigned int cal_rs(unsigned int ins) {
    unsigned int rs = ins & 0x03e00000;
    rs = rs >> 21;
    rs = rs & 0x0000001f;
    return rs;
}
unsigned int cal_rt(unsigned int ins) {
    unsigned int rt = ins & 0x001f0000;
    rt = rt >> 16;
    rt = rt & 0x0000001f;
    return rt;
}
unsigned int cal_rd(unsigned int ins) {
    unsigned int rd = ins & 0x0000f800;
    rd = rd >> 11;
    rd = rd & 0x0000001f;
    return rd;
}
unsigned int cal_shamt(unsigned int ins) {
    unsigned int sh = ins & 0x000007c0;
    sh = sh >> 6;
    sh = sh & 0x0000001f;
    return sh;
}
unsigned int cal_func(unsigned int ins) {
    ins = ins & 0x0000003f;
    return ins;
}
int cal_imm(unsigned int ins, bool a) { // a = TRUE(SIGNED) FALSE(UNSIGNED)
    if (a == false) {//UNSIG
        ins = ins & 0x0000ffff;
        return ins;
    }
    else {//SIG
        ins = ins & 0x0000ffff;
        int b = ins >> 15;
        if (b == 1) {
            int c = ins | 0xffff0000;
            return c;
        }
        else {
            return ins;
        }
    }
}
int mux(int a, int b, unsigned int sig) {
    if (sig == 0) return a;
    else return b;
}
unsigned int op_ALU(unsigned int a, unsigned int b, int c, unsigned int sig) {
    if (sig == 0) {
        return a + b;
    }
    else if (sig == 1) {
        return  a - b;
    }
    else if (sig == 2) {
        return  a & b;
    }
    else if (sig == 3) {
        if (a == b) return 1;
        else return 0;
    }
    else if (sig == 4) {
        if (a != b) return 1;
        else return 0;
    }
    else if (sig == 5) {
        return !(a | b);
    }
    else if (sig == 6) {
        return  (a | b);
    }
    else if (sig == 7) {
        return (a < b) ? 1 : 0;
    }
    else if (sig == 8) {
        return  b << c;
    }
    else if (sig == 9) {
        return  b >> c;
    }
    else if (sig == 10) {
        return  0;
    }
}

void init_cpu() {
    pc = 0;
    memset(R, 0x0, sizeof(R));
    R[31] = 0xffffffff; // LINK REG
    R[29] = 0x200000;   // STACK POINTER
}
void update_fetch(struct Fetch_Decode_latch& in, struct Fetch_Decode_latch& out) {
    if ((in.valid == 1)&&(out.valid == 1)) {
        out.inst = in.inst;
        out.pc_value = in.pc_value;
        decode_first = 0;
        // printf("update fetch %d 0x%x \n\n", in.pc_value, in.inst);
        return;
    }
    else {
        out.valid = 1;
        // printf("skip fetch %d \n\n", in.pc_value);
        return;
    }
}
int fetch_ins(struct Fetch_Decode_latch& in) {
    if (pc == 0xffffffff) {
        in.valid = 0;
        memset(&in, 0, sizeof(in));
        printf(" end after 3 clock found in %d 0x%x \n\n",in.pc_value,in.inst);
        return -1;
    }
        in.valid = 1; // Fetch UPDATE 활성화

        in.pc_value = pc;
        in.inst = ch[pc / 4];
        n_ins++;

        // jumpLib 검색 및 브랜치 prediction
        int flag = 0;
        int result = 0;
        for (int i = 0; i < JumpIter; i++) {
            if (pc == JumpLib[i][0]) {
                flag = 1;
                result = i;
                break;
            }
        }
        if (flag == 1) {
            switch (JumpLib[result][3])
            {
            case 0 :  // unconditional jump
                pc = JumpLib[result][2];
                break;
            case 1 : // Forward Taken
                pc = pc + 4; // always not taken
                break;
            case 2 : // Backward Taken
                pc = JumpLib[result][2]; // always taken
                break;
            default: 
                printf("error is detected -> out of range (JumpLib[%d][3])\n", result);
                return -1;
                break;
            }
        }
        else {
            pc = pc + 4; // 일반적인 instruction
        }
        
        printf("fetch complete %d 0x%x \n", in.pc_value, in.inst);
        return 1;
}
void update_decode(struct Decode_Execute_latch& in, struct Decode_Execute_latch& out) {
    if ((in.valid == 1) && (out.valid == 1)) {
        out.opcode = in.opcode;
        out.pc_value = in.pc_value;
        out.ReadData1 = in.ReadData1;
        out.ReadData2 = in.ReadData2;
        out.MemWriteData = in.MemWriteData;
        out.WriteBackNum = in.WriteBackNum;
        out.cs = in.cs;
        out.imm = in.imm;
        out.rt = in.rt;
        out.rd = in.rd;
        out.rs = in.rs;
        out.funct = in.funct;
        out.shamt = in.shamt;
        execute_first = 0;
        //printf("decode update complete %d \n\n", in.pc_value);
        return;
    }
    else {
        //printf("decode update failed %d \n\n", in.pc_value);
        return;
    }
}
int decode_ins(struct Fetch_Decode_latch& out, struct Decode_Execute_latch& in) {
    out.valid = 1;
    if (decode_first == 1) {
        //printf("skip decode %d \n\n", out.pc_value);
        return 1;
    }
    in.valid = 1;

    // value setting
    in.pc_value = out.pc_value;
    in.opcode = cal_opc(out.inst);
    in.rs = cal_rs(out.inst);
    in.rt = cal_rt(out.inst);
    in.rd = cal_rd(out.inst);
    in.shamt = cal_shamt(out.inst);
    if (in.opcode == 0) in.funct = cal_func(out.inst);
    else in.funct = 0xff;
    if ((in.opcode == 0xc) || (in.opcode == 0xd)) in.imm = cal_imm(out.inst, false);
    else in.imm = cal_imm(out.inst, true);
    printf("op: %d, rs: %d rt: %d rd: %d shamt: %d funct: %d imm: %d \n", in.opcode, in.rs, in.rt, in.rd, in.shamt, in.funct, in.imm);

    // control signal setting
    if (in.opcode == 0x0)  in.cs.regDst = 1; //RegDest
    else in.cs.regDst = 0;
    { // ALUop
        if ((in.funct == 0x20) || (in.funct == 0x21) || (in.opcode == 0x8) || (in.opcode == 0x9) || (in.opcode == 0x30) || (in.opcode == 0x23) || (in.opcode == 0x2b)) in.cs.aluop = 0; // add
        else if ((in.funct == 0x22) || (in.funct == 0x23)) in.cs.aluop = 1; // sub
        else if ((in.funct == 0x24) || (in.opcode == 0xc)) in.cs.aluop = 2; // and
        else if (in.opcode == 0x4) in.cs.aluop = 3; // beq
        else if (in.opcode == 0x5) in.cs.aluop = 4; // bne
        else if ((in.funct == 0x27)) in.cs.aluop = 5; // Nor
        else if ((in.funct == 0x25) || (in.opcode == 0xd)) in.cs.aluop = 6; // or
        else if ((in.funct == 0x2a) || (in.funct == 0x2b) || (in.opcode == 0x0a) || (in.opcode == 0x0b)) in.cs.aluop = 7;// slt
        else if (in.funct == 0x0) in.cs.aluop = 8;// shift left
        else if (in.funct == 0x2) in.cs.aluop = 9; // shift right
        else in.cs.aluop = 10; // DON'T CARE
    }
    if ((in.opcode == 0x0) || (in.opcode == 0x04) || (in.opcode == 0x05)) in.cs.alusrc = 0; // AluSrc
    else in.cs.alusrc = 1;
    if ((in.opcode == 0x02) || (in.opcode == 0x03) || (in.funct == 0x08) || (in.opcode == 0x04) || (in.opcode == 0x05) || (in.opcode == 0x2b)) in.cs.regWrite = 0; //RegWrite
    else in.cs.regWrite = 1;
    if (in.opcode == 0x2b) in.cs.memWrite = 1; // MemWrite
    else in.cs.memWrite = 0;
    if ((in.opcode == 0x30) || (in.opcode == 0x23)) {
        in.cs.memToReg = 1;   //MemRead, MemToReg
    }
    else in.cs.memToReg = 0;
    if ((in.opcode == 0x04) || (in.opcode == 0x05)) {
        in.cs.branchJ = 1; // Branch
        n_branch++;
    }
    else in.cs.branchJ = 0;
    if ((in.opcode == 0x2) || (in.opcode == 0x3) || (in.opcode == 0xf) || (in.funct == 0x00) || (in.funct == 0x02)) in.cs.rsUse = 0; // data dependency check 
    else in.cs.rsUse = 1;
    if ((in.opcode == 0x2) || (in.opcode == 0x3) || (in.funct == 0x08)) in.cs.rtUse = 0; // data dependency check 
    else in.cs.rtUse = 1;

    //Register value move
    in.ReadData1 = R[in.rs];
    in.ReadData2 = mux(R[in.rt], in.imm, in.cs.alusrc);
    in.WriteBackNum = mux(in.rt, in.rd, in.cs.regDst);
    in.MemWriteData = R[in.rt];
    printf("data1: %x data2: %d writebacknum: %d memwritedata: %x \n", in.ReadData1, in.ReadData2, in.WriteBackNum, in.MemWriteData);

    // Unconditional jump
    if (in.opcode == 0x2) {
        int a = cal_jump(out.pc_value, out.inst);
        n_jump++;
        if (a != (pc-4)) {
            memset(&out, 0, sizeof(out));
            out.valid = 0; // fetch update 실행 금지
            decode_first = 1; // decode 실행 금지
            printf("Unconditional Jump Stall \n\n");
            n_ins--;
            pc = a;
        }
        if (JumpIter == 0) {
            JumpLib[0][0] = in.pc_value;
            JumpLib[0][1] = in.opcode;
            JumpLib[0][2] = a;
            JumpLib[0][3] = 0;
            JumpIter++;
        }
        else if (JumpIter > 0) {
            int flag = 0;
            for (int i = 0; i < JumpIter; i++) {
                if (in.pc_value == JumpLib[i][0]) {
                    flag = 1;
                    break;
                }
            }
            if (flag == 0) {
                JumpLib[JumpIter][0] = in.pc_value;
                JumpLib[JumpIter][1] = in.opcode;
                JumpLib[JumpIter][2] = a;
                JumpLib[JumpIter][3] = 0;
                JumpIter++;
            }
        }
    }
    else if (in.opcode == 0x3) {
        R[31] = out.pc_value + 8; // 내부 WriteBack
        n_jump++;
        int a = cal_jump(out.pc_value, out.inst);
        if (a != (pc - 4)) {

            memset(&out, 0, sizeof(out));
            out.valid = 0; // fetch update 실행 금지
            decode_first = 1; // decode 실행 금지
            printf("Unconditional Jump Stall \n\n");
            n_ins--;
            pc = a;
        }
        if (JumpIter == 0) {
            JumpLib[0][0] = in.pc_value;
            JumpLib[0][1] = in.opcode;
            JumpLib[0][2] = a;
            JumpLib[0][3] = 0;
            JumpIter++;
        }
        else if (JumpIter > 0) {
            int flag = 0;
            for (int i = 0; i < JumpIter; i++) {
                if (in.pc_value == JumpLib[i][0]) {
                    flag = 1;
                    break;
                }
            }
            if (flag == 0) {
                JumpLib[JumpIter][0] = in.pc_value;
                JumpLib[JumpIter][1] = in.opcode;
                JumpLib[JumpIter][2] = a;
                JumpLib[JumpIter][3] = 0;
                JumpIter++;
            }
        }
    }
    else if ((in.funct == 0x8) && (in.opcode == 0x0)) {
        int a = R[in.rs];
        n_jump++;
        if (a != (pc - 4)) {
            memset(&out, 0, sizeof(out));
            out.valid = 0; // fetch update 실행 금지
            decode_first = 1; // decode 실행 금지
            printf("Unconditional Jump Stall \n\n");
            n_ins--;
            pc = a;
        }
        if (JumpIter == 0) {
            JumpLib[0][0] = in.pc_value;
            JumpLib[0][1] = in.opcode;
            JumpLib[0][2] = a;
            JumpLib[0][3] = 0;
            JumpIter++;
        }
        else if (JumpIter > 0) {
            int flag = 0;
            for (int i = 0; i < JumpIter; i++) {
                if (in.pc_value == JumpLib[i][0]) {
                    flag = 1;
                    break;
                }
            }
            if (flag == 0) {
                JumpLib[JumpIter][0] = in.pc_value;
                JumpLib[JumpIter][1] = in.opcode;
                JumpLib[JumpIter][2] = a;
                JumpLib[JumpIter][3] = 0;
                JumpIter++;
            }
        }
    }
    printf("decode complete %d \n",out.pc_value);
    return 1;
}
void update_Execute(struct Execute_MemAccess_latch& in, struct Execute_MemAccess_latch& out) {
    if ((in.valid == 1) && (out.valid == 1)) {
        out.pc_value = in.pc_value;
        out.AluResult = in.AluResult;
        out.rs = in.rs;
        out.rt = in.rt;
        out.rd = in.rd;
        out.funct = in.funct;
        out.MemWriteData = in.MemWriteData;
        out.WriteBackNum = in.WriteBackNum;
        out.cs = in.cs;
        memaccess_first = 0;
        //printf("execute update complete %d \n\n", out.pc_value);
        return;
    }
    else {
        //printf("execute update failed %d \n\n", out.pc_value);
        return;
    }
}
int execute_ins(struct Fetch_Decode_latch& dout,struct Decode_Execute_latch& din,struct Decode_Execute_latch& out,struct Execute_MemAccess_latch& in) {
    out.valid = 1;
    if (execute_first == 1) {
        //printf("skip execute %d \n\n", out.pc_value);
        return 1;
    }
    in.valid = 1;

    in.AluResult = op_ALU(out.ReadData1, out.ReadData2, out.shamt , out.cs.aluop);
    if (out.opcode == 0x0f) {
        in.AluResult = out.imm << 16; // load upper imm
    }
    in.cs = out.cs;
    in.MemWriteData = out.MemWriteData;
    in.WriteBackNum = out.WriteBackNum;
    in.rs = out.rs;
    in.rt = out.rt;
    in.funct = out.funct;
    in.pc_value = out.pc_value;
    
    // branch
    if ((out.cs.branchJ == 1) && (in.AluResult == 1)) {
        n_taken++;
        int BAddr = out.pc_value + 4 + (out.imm << 2);
        if (BAddr != (din.pc_value)) {
            memset(&dout, 0, sizeof(dout));
            memset(&out, 0, sizeof(out));
            dout.valid = 0;
            out.valid = 0;
            pc = BAddr;
            printf("branch prediction is failed \n\n");
            branchNotPredict++;
        }
        else branchPredict++;
        if (JumpIter == 0) {
            JumpLib[0][0] = in.pc_value;
            JumpLib[0][1] = out.opcode;
            JumpLib[0][2] = BAddr;
            if ((out.imm << 2) > 0) {
                JumpLib[0][3] = 1; // 진행 방향 아래
            }else JumpLib[0][3] = 2; // 진행 방향 다시 위로
            JumpIter++;
        }
        else if (JumpIter > 0) {
            int flag = 0;
            for (int i = 0; i < JumpIter; i++) {
                if (in.pc_value == JumpLib[i][0]) {
                    flag = 1;
                    break;
                }
            }
            if (flag == 0) {
                JumpLib[JumpIter][0] = in.pc_value;
                JumpLib[JumpIter][1] = out.opcode;
                JumpLib[JumpIter][2] = BAddr;
                if ((out.imm << 2) > 0) {
                    JumpLib[0][3] = 1; // 진행 방향 아래
                }
                else JumpLib[0][3] = 2; // 진행 방향 다시 위로
                JumpIter++;
            }
        }
    }
    else if ((out.cs.branchJ == 1) && (in.AluResult == 0)) {
        if ((in.pc_value + 4) != din.pc_value) {
            memset(&dout, 0, sizeof(dout));
            memset(&out, 0, sizeof(out));
            dout.valid = 0;
            out.valid = 0;
            pc = in.pc_value + 4;
            printf("branch prediction is failed \n\n");
            branchNotPredict++;
        }else branchPredict++;
        int BAddr = out.pc_value + 4 + (out.imm << 2);
        if (JumpIter == 0) {
            JumpLib[0][0] = in.pc_value;
            JumpLib[0][1] = out.opcode;
            JumpLib[0][2] = BAddr;
            if ((out.imm << 2) > 0) {
                JumpLib[0][3] = 1; // Forward Branch
            }
            else JumpLib[0][3] = 2; // Backward Branch
            JumpIter++;
        }
        else if (JumpIter > 0) {
            int flag = 0;
            for (int i = 0; i < JumpIter; i++) {
                if (in.pc_value == JumpLib[i][0]) {
                    flag = 1;
                    break;
                }
            }
            if (flag == 0) {
                JumpLib[JumpIter][0] = in.pc_value;
                JumpLib[JumpIter][1] = out.opcode;
                JumpLib[JumpIter][2] = BAddr;
                if ((out.imm << 2) > 0) {
                    JumpLib[0][3] = 1; // Forward Branch
                }
                else JumpLib[0][3] = 2; // Backward Branch
                JumpIter++;
            }
        }
        n_not_taken++;
    }
    
    // data dependency check
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rs) && (out.cs.rsUse == 1)) {
        din.ReadData1 = in.AluResult;
        if ((din.funct == 0x8) && (din.opcode == 0x0)) pc = in.AluResult; // register jump case update.
        rs_dist = 1;
        printf("rs dependency solved\n");
    } 
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rt) && (out.cs.rtUse == 1)) {
        if (din.cs.alusrc == 0) din.ReadData2 = in.AluResult;
        din.MemWriteData = in.AluResult;
        rt_dist = 1;
        printf("rt dependency solved\n");
    }

    printf("alu result: %d memwritedata: %d , writebacknum:%d rs: %d rt: %d pc value: %d \n\n", in.AluResult, in.MemWriteData, in.WriteBackNum, in.rs, in.rt, in.pc_value);
    return 1;
}
void update_MemAccess(struct MemAccess_WriteBack_latch& in, struct MemAccess_WriteBack_latch& out) {
    if ((in.valid == 1) && (out.valid == 1)) {
        out.pc_value = in.pc_value;
        out.Data = in.Data;
        out.WriteBackNum = in.WriteBackNum;
        out.cs= in.cs;
        writeback_first = 0;
        //printf("memaccess update complete %d \n\n", out.pc_value);
        return;
    }
    else {
        //printf("memaccess update failed %d \n\n", out.pc_value);
        return;
    }
}
int memAccess_ins(struct Decode_Execute_latch& din,struct Execute_MemAccess_latch& out, struct MemAccess_WriteBack_latch& in) {
    out.valid = 1;
    if (memaccess_first == 1) {
        //printf("skip memaccess %d \n\n", out.pc_value);
        return 1;
    }
    in.valid = 1;
    
    if (out.cs.memWrite == 1) {
        ch[out.AluResult] = out.MemWriteData;
        n_mem++;
        printf("changed state -> M[%08x] = %x\n", out.AluResult, out.MemWriteData);
    }
    if (out.cs.memToReg == 1) n_mem++;
    in.Data = mux(out.AluResult, ch[out.AluResult], out.cs.memToReg);
    in.cs = out.cs;
    in.pc_value = out.pc_value;
    in.WriteBackNum = out.WriteBackNum;
    printf("data: %d pc_value: %d writebacknum: %d \n\n", in.Data, in.pc_value, in.WriteBackNum);
    

    // data dependency check
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rs) && (out.cs.rsUse == 1) && (rs_dist > 1)) {
        din.ReadData1 = in.Data;
        if ((din.funct == 0x8) && (din.opcode == 0x0)) pc = in.Data; // register jump case update.
        rs_dist = 2;
        printf("rs dependency solved\n");
    }
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rt) && (out.cs.rtUse == 1) && (rt_dist > 1)) {
        if (din.cs.alusrc == 0) din.ReadData2 = in.Data;
        din.MemWriteData = in.Data;
        rt_dist = 2;
        printf("rt dependency solved\n");
    }
    return 1;
}

int writeBack_ins(struct Decode_Execute_latch& din,struct MemAccess_WriteBack_latch& out) {
    out.valid = 1;
    if (writeback_first == 1) {
        //printf("skip writeback %d \n\n", out.pc_value);
        return 1;
    }

    if (out.cs.regWrite == 1) {
        R[out.WriteBackNum] = out.Data;
        n_reg++;
        printf("changed state -> R[%d] = %x\n", out.WriteBackNum, out.Data);
        printf("complete writeback %d \n\n", out.pc_value);
    }
    else {
        printf("complete writeback %d \n\n", out.pc_value);
    }
    // data dependency check
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rs) && (out.cs.rsUse == 1) && (rs_dist > 2)) {
        din.ReadData1 = out.Data;
        if ((din.funct == 0x8) && (din.opcode == 0x0)) pc = out.Data; // register jump case update.
        printf("rs dependency solved\n");
    }
    if ((out.cs.regWrite == 1) && (out.WriteBackNum == din.rt) && (out.cs.rtUse == 1) && (rt_dist > 2)) {
        if (din.cs.alusrc == 0) din.ReadData2 = out.Data;
        din.MemWriteData = out.Data;
        printf("rt dependency solved\n");
    }
    return 1;
}

int main() {
    // 시간 측정
    time_t start, end;
    double result;

    // 파일 포인터, Latch 설정 
    FILE* in;
    struct Fetch_Decode_latch a[2]; // [0] 은 in [1] 은 out
    struct Decode_Execute_latch b[2];
    struct Execute_MemAccess_latch c[2];
    struct MemAccess_WriteBack_latch d[2];

    int keep = 0;
    int n_clock = 0; // 걸린 클락 수

    // 인스트럭션 -> 메모리 입력
    if ((in = fopen("input4.bin", "rb")) == NULL) {  // 파일 오픈
        fputs("파일이 존재하지 않습니다", stderr);
        return -1;
    }
    while (feof(in) == 0) {
        int arr = 0;                       // load ins'
        fread(&arr, sizeof(int), 1, in);
        ch[pc / 4] = ntohl(arr);
        printf("%d : %08x \n", pc, ch[pc / 4]);
        pc += 4;
    }
    fclose(in); // 파일 닫기 

    // 프로그램 실행 시간 측정 
    start = clock();
    // 프로그램 시작
    init_cpu(); // 전역 함수 초기화 
    memset(a, 0, sizeof(a)); // latch 변수 0 으로 초기화
    memset(b, 0, sizeof(b));
    memset(c, 0, sizeof(c));
    memset(d, 0, sizeof(d));
    do {
        n_clock++;

        update_fetch(a[0], a[1]);
        int quit = fetch_ins(a[0]);
        if (quit < 0) keep++; // pc가 종료 값이면 keep +1,

        update_decode(b[0], b[1]);
        decode_ins(a[1],b[0]);

        update_Execute(c[0], c[1]);
        execute_ins(a[1],b[0],b[1],c[0]);

        update_MemAccess(d[0], d[1]);
        memAccess_ins(b[0],c[1],d[0]);

        writeBack_ins(b[0],d[1]);
        // Data Dependency Check 변수 초기화
        rs_dist = 4;
        rt_dist = 4;
        printf("%d \n", keep);
        printf("---------------------------------");
        printf("---------------------------------");
    } while (keep != 2);
    end = clock();

    result = (double)(end - start);
    printf("Program Execute Time : %lf seconds\n", result/CLOCKS_PER_SEC );
    printf("Final Return Value : %d\n\n", R[2]);
    printf("Number of Instruction : %d\n", n_ins);
    printf("Number of Memory Access : %d\n", n_mem);
    printf("Number of Register WriteBack : %d\n", n_reg);
    printf("Number of Unconditional Jump : %d\n", n_jump);
    printf("Number of Conditional Jump : %d\n", n_branch);
    printf("Number of Taken branch : %d\n", n_taken);
    printf("Number of Not Taken branch : %d\n", n_not_taken);
    printf("branch Predict : %d\n", branchPredict);
    printf("branch not Predict : %d\n", branchNotPredict);
    printf("Branch Prediction Percentage : %f\n", ((double)branchPredict / (double)(branchNotPredict + branchPredict)));
    printf("Number of clock : %d\n", n_clock);
    printf("MIPS END");
    
    return 0;
}