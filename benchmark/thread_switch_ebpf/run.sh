make

sudo python3 monitor_task.py config/config_rt.yaml > log/event_log_rt.txt
sudo python3 monitor_task.py config/config_pmu.yaml > log/event_log_pmu.txt

python3 plot_compare.py log/event_log_rt.txt log/event_log_pmu.txt
