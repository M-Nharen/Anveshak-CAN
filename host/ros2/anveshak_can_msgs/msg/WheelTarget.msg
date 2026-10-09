# Target for one drive node (it has two motor channels, A and B).
uint8 MODE_COAST=0
uint8 MODE_BRAKE=1
uint8 MODE_VELOCITY=2
uint8 MODE_DUTY=3

uint8 mode 2
# rad/s in MODE_VELOCITY, percent duty in MODE_DUTY
float64 channel_a 0.0
float64 channel_b 0.0
# rad/s^2 limit sent with every command
float64 accel_limit 10.0
