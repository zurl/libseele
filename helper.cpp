//
// Created by zcy on 2016-09-15.
//
#include "helper.h"

std::string Helper::itos(unsigned long long int a){
    std::stringstream ss;
    ss<<a;
    return ss.str();
}
int Helper::stoi(std::string & a){
    std::stringstream ss;
    ss<<a;
    int i;
    ss>>i;
    return i;
}