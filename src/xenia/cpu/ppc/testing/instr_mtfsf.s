# FM bit 7 (the LSB) selects field 7: only RN changes.
test_mtfsf_1:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x0000000090000003
  mtfsf 0xFF, f1
  mtfsf 0x01, f2
  mffs f3
  mtfsf 0xFF, f1
  blr
  #_ REGISTER_OUT f3 0x0000000000000003

# FM bit 0 (the MSB) selects field 0: only FX..OX change.
test_mtfsf_2:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x0000000090000003
  mtfsf 0xFF, f1
  mtfsf 0x80, f2
  mffs f3
  mtfsf 0xFF, f1
  blr
  #_ REGISTER_OUT f3 0x0000000090000000
