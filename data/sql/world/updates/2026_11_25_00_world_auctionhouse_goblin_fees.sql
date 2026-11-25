-- The neutral auction house (ID 7), which every auctioneer uses under AllowTwoSide.Interaction.Auction = 1, at the faction houses' fees (deposit 5, cut 5 %); name and mask as in the client's AuctionHouse.dbc
DELETE FROM `auctionhouse_dbc` WHERE `ID` = 7;
INSERT INTO `auctionhouse_dbc` (`ID`, `FactionID`, `DepositRate`, `ConsignmentRate`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`) VALUES
(7, 369, 5, 5, 'Blackwater Auction House', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', 16712190);
