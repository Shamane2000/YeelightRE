// addr=1f008784
// functionAt=null
// functionContaining=null
// createFunction=OK ota_worker_task_1f008784 body=518
// 
/* FUNCTION ota_worker_task_1f008784 @ 0x1f008784 size=518 */

uint ota_worker_task_1f008784(int param_1,int param_2,int param_3,char *param_4,byte param_5)

{
  bool bVar1;
  char cVar2;
  bool bVar3;
  bool bVar4;
  int *piVar5;
  int *piVar6;
  int *piVar7;
  int iVar8;
  int iVar9;
  char *pcVar10;
  uint uVar11;
  char acStack_30 [12];
  
  iVar8 = FUN_1f0084f8();
  if (iVar8 == 0) {
    FUN_1f00b400();
    piVar7 = DAT_1f008994;
    piVar6 = DAT_1f008990;
    piVar5 = DAT_1f00898c;
    *DAT_1f00898c = 0;
    *piVar6 = 0;
    *piVar7 = 0;
    if ((param_1 != 0) && (iVar8 = FUN_1f042f5c(param_1,DAT_1f008998,4), iVar8 == 0)) {
      *piVar7 = 0x20;
    }
    if ((param_2 != 0) && (iVar8 = FUN_1f042f5c(param_2,DAT_1f008998,4), iVar8 == 0)) {
      *piVar6 = 0x10;
    }
    if ((param_3 != 0) && (iVar8 = FUN_1f042f5c(param_3,DAT_1f008998,4), iVar8 == 0)) {
      *piVar5 = 0x140;
    }
    if (*piVar7 + *piVar6 + *piVar5 != 0) {
      if ((param_4 == (char *)0x0) ||
         (((cVar2 = *param_4, pcVar10 = param_4, cVar2 != 'a' && (cVar2 != 'w')) && (cVar2 != 'm')))
         ) {
        pcVar10 = DAT_1f00899c;
      }
      FUN_1f042fd0(acStack_30,pcVar10,8);
      if (-1 < (int)((uint)param_5 << 0x1e)) {
        FUN_1f008710();
      }
      iVar8 = FUN_1f043f7c();
      iVar9 = FUN_1f04400c();
      bVar1 = iVar8 == 0;
      bVar3 = iVar9 == 0;
      bVar4 = bVar1 || bVar3;
      iVar8 = 0;
      do {
        if ((*piVar7 != 0) && (acStack_30[iVar8] == 'a')) {
          if (*DAT_1f0089a0 != 1) {
            FUN_1f0085b4(1);
          }
          if (bVar1 || bVar3) {
            iVar9 = FUN_1f008344();
          }
          else {
            iVar9 = FUN_1f008304(param_1);
          }
          if (iVar9 == 0) {
            *DAT_1f0089a4 = 100;
            goto LAB_1f008888;
          }
LAB_1f0088fc:
          iVar8 = 2;
          goto LAB_1f008900;
        }
LAB_1f008888:
        if ((*piVar6 != 0) && (acStack_30[iVar8] == 'w')) {
          if (*DAT_1f0089a0 != 1) {
            FUN_1f0085b4(1);
          }
          if (bVar1 || bVar3) {
            iVar9 = FUN_1f0083fe();
          }
          else {
            iVar9 = FUN_1f00835a(param_2);
          }
          if (iVar9 != 0) goto LAB_1f0088fc;
          *DAT_1f0089a8 = 100;
        }
        if ((*piVar5 != 0) && (acStack_30[iVar8] == 'm')) {
          if (*DAT_1f0089a0 != 1) {
            FUN_1f0085b4(1);
          }
          if (bVar1 || bVar3) {
            iVar9 = FUN_1f008474();
          }
          else {
            iVar9 = FUN_1f008414(param_3);
          }
          if (iVar9 != 0) goto LAB_1f0088fc;
        }
        iVar8 = iVar8 + 1;
      } while (iVar8 != 3);
      iVar8 = 1;
      goto LAB_1f008900;
    }
  }
  bVar4 = false;
  iVar8 = 0;
LAB_1f008900:
  if (param_1 != 0) {
    FUN_1f058de8(param_1);
  }
  if (param_2 != 0) {
    FUN_1f058de8(param_2);
  }
  if (param_3 != 0) {
    FUN_1f058de8(param_3);
  }
  if (param_4 != (char *)0x0) {
    FUN_1f058de8(param_4);
  }
  if (iVar8 == 2) {
    FUN_1f0085b4(5);
    FUN_1f008758();
    return 0xffffffff;
  }
  if (iVar8 == 1) {
    uVar11 = param_5 & 1;
    if (!bVar4) {
      FUN_1f0085b4(1);
      uVar11 = FUN_1f008638(acStack_30,uVar11);
      FUN_1f008758();
      return uVar11;
    }
    if ((param_5 & 1) == 0) {
      FUN_1f0085b4(3);
      FUN_1f0582f4(6000);
      FUN_1f008758();
      FUN_1f041d38();
      return uVar11;
    }
    FUN_1f0085b4(4);
  }
  return 0;
}


